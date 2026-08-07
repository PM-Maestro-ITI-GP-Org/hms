/*
 * shell.c
 * Persistent interactive SSH shell sessions for guests.
 *
 * Each session is a forked "ssh -T user@guest" child with a pipe pair for
 * stdin/stdout. A reader thread forwards the child's output to MQTT as
 * shell_out chunks; when the child exits the slot is released and a
 * shell_closed message is published. All sessions are independent, so the
 * MQTT command loop is never blocked.
 */
#include "shell.h"
#include "client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>

/* Close a session when nothing is written or read for this long. This is
 * the backstop for a GUI that disappears without sending shellclose. */
#define SHELL_IDLE_TIMEOUT 60

typedef struct {
    char         guest_id[GUEST_ID_LEN];
    pid_t        pid;
    int          in_fd;      /* write here -> ssh stdin  */
    int          out_fd;     /* read ssh stdout/stderr   */
    volatile int alive;
    pthread_t    reader;
    hms_mqtt_t  *mqtt;
    time_t       last_activity;
} ShellSession;

static ShellSession s_sessions[SHELL_MAX_SESSIONS];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_monitor_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_monitor_started = 0;

static int shell_escape(const char *in, char *out, size_t out_sz)
{
    int j = 0;
    for (int i = 0; in[i] && j < (int)out_sz - 8; i++) {
        char c = in[i];
        if (c == '"' || c == '\\')      out[j++] = '\\', out[j++] = c;
        else if (c == '\n')             out[j++] = '\\', out[j++] = 'n';
        else if (c == '\r')             out[j++] = '\\', out[j++] = 'r';
        else if (c == '\t')             out[j++] = '\\', out[j++] = 't';
        else if ((unsigned char)c < 0x20) {
            int n = snprintf(out + j, out_sz - j, "\\u%04x", (unsigned char)c);
            j += n;
        } else out[j++] = c;
    }
    out[j] = '\0';
    return j;
}

static void publish_chunk(hms_mqtt_t *mqtt, const char *guest_id,
                          const char *data)
{
    char esc[4096];
    shell_escape(data, esc, sizeof(esc));
    char buf[4300];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"shell_out\",\"guest\":\"%s\",\"data\":\"%s\"}",
             guest_id, esc);
    hms_mqtt_publish_status(mqtt, buf);
}

static void publish_shell_state(hms_mqtt_t *mqtt, const char *guest_id,
                                const char *state, const char *msg)
{
    char esc[1024];
    shell_escape(msg, esc, sizeof(esc));
    char buf[1400];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"guest\":\"%s\",\"msg\":\"%s\"}",
             state, guest_id, esc);
    hms_mqtt_publish_status(mqtt, buf);
}

static void *shell_reader(void *arg)
{
    ShellSession *s = (ShellSession *)arg;
    char chunk[512];
    char buf[513];

    while (s->alive) {
        ssize_t n = read(s->out_fd, chunk, sizeof(chunk));
        if (n > 0) {
            memcpy(buf, chunk, n);
            buf[n] = '\0';
            s->last_activity = time(NULL);
            publish_chunk(s->mqtt, s->guest_id, buf);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break; /* EOF or error: session ended */
    }

    /* Close everything and free the slot. */
    pthread_mutex_lock(&s_lock);
    s->alive = 0;
    if (s->out_fd >= 0) { close(s->out_fd); s->out_fd = -1; }
    if (s->in_fd >= 0)  { close(s->in_fd);  s->in_fd = -1; }
    if (s->pid > 0)     { kill(s->pid, SIGKILL); s->pid = 0; }
    pthread_mutex_unlock(&s_lock);

    publish_shell_state(s->mqtt, s->guest_id, "shell_closed", "session ended");

    pthread_mutex_lock(&s_lock);
    s->guest_id[0] = '\0';
    pthread_mutex_unlock(&s_lock);
    return NULL;
}

/* Close sessions that saw no traffic for SHELL_IDLE_TIMEOUT seconds.
 * Covers a GUI that closed without sending shellclose (app killed, broker
 * dropped, ...) — the ssh child dies with the session and shell_closed is
 * published by the reader thread. */
static void *shell_monitor(void *arg)
{
    (void)arg;
    while (1) {
        sleep(5);
        char ids[SHELL_MAX_SESSIONS][GUEST_ID_LEN];
        int n = 0;
        time_t now = time(NULL);

        pthread_mutex_lock(&s_lock);
        for (int i = 0; i < SHELL_MAX_SESSIONS; i++) {
            if (s_sessions[i].alive
                && now - s_sessions[i].last_activity > SHELL_IDLE_TIMEOUT) {
                snprintf(ids[n], GUEST_ID_LEN, "%.31s", s_sessions[i].guest_id);
                n++;
            }
        }
        pthread_mutex_unlock(&s_lock);

        for (int i = 0; i < n; i++) {
            printf("[hms] shell session '%s' idle timeout — closing\n", ids[i]);
            shell_close(ids[i]);
        }
    }
    return NULL;
}

int shell_open(hms_mqtt_t *mqtt, const Guest *g,
               char *errbuf, size_t errbuf_sz)
{
    if (!g->ip[0]) { snprintf(errbuf, errbuf_sz, "guest has no IP address"); return -1; }
    if (g->state != GUEST_RUNNING) { snprintf(errbuf, errbuf_sz, "guest is not running"); return -1; }

    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < SHELL_MAX_SESSIONS; i++) {
        if (s_sessions[i].alive && strcmp(s_sessions[i].guest_id, g->id) == 0) {
            pthread_mutex_unlock(&s_lock);
            snprintf(errbuf, errbuf_sz, "a shell is already open for this guest");
            return -1;
        }
    }
    pthread_mutex_unlock(&s_lock);

    /*
     * ssh_build_opts() supplies the host-key policy, the timeout, the port and
     * -i only when the guest actually has a key file.
     *
     * The hand-rolled string this replaces got both halves wrong. It passed
     * StrictHostKeyChecking=no together with UserKnownHostsFile=/dev/null --
     * the pair client.c was deliberately moved away from, which accepts
     * whatever answers on the address and keeps no record to compare next
     * time. And it always passed "-i %s": for a guest authenticating by
     * password, ssh_key is empty, so the option became a bare "-i" that
     * swallowed the following "user@host" as its filename argument and left
     * ssh with no destination at all. Opening a shell on such a guest could
     * not work.
     */
    char ssh_opts[1024];
    ssh_build_opts(g, ssh_opts, sizeof(ssh_opts), 0);

    char sshcmd[2048];
    snprintf(sshcmd, sizeof(sshcmd),
             "ssh -T %s -o ServerAliveInterval=10 -o ServerAliveCountMax=60 %s@%s",
             ssh_opts, g->ssh_user, g->ip);

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        snprintf(errbuf, errbuf_sz, "pipe() failed: %s", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        snprintf(errbuf, errbuf_sz, "fork() failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        dup2(out_pipe[1], 2);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execl("/bin/sh", "sh", "-c", sshcmd, (char *)NULL);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);

    pthread_mutex_lock(&s_lock);
    ShellSession *s = NULL;
    for (int i = 0; i < SHELL_MAX_SESSIONS; i++) {
        if (!s_sessions[i].alive && s_sessions[i].guest_id[0] == '\0') {
            s = &s_sessions[i];
            break;
        }
    }
    if (!s) {
        pthread_mutex_unlock(&s_lock);
        kill(pid, SIGKILL);
        close(in_pipe[1]); close(out_pipe[0]);
        snprintf(errbuf, errbuf_sz, "no free shell session slot");
        return -1;
    }

    s->mqtt = mqtt;
    s->pid = pid;
    s->in_fd = in_pipe[1];
    s->out_fd = out_pipe[0];
    s->alive = 1;
    s->last_activity = time(NULL);
    snprintf(s->guest_id, sizeof(s->guest_id), "%s", g->id);

    if (pthread_create(&s->reader, NULL, shell_reader, s) != 0) {
        s->alive = 0;
        s->guest_id[0] = '\0';
        pthread_mutex_unlock(&s_lock);
        kill(pid, SIGKILL);
        close(in_pipe[1]); close(out_pipe[0]);
        snprintf(errbuf, errbuf_sz, "failed to start reader thread");
        return -1;
    }
    pthread_detach(s->reader);
    pthread_mutex_unlock(&s_lock);

    printf("[hms] shell session opened for '%s' (pid %d)\n", g->id, pid);
    publish_shell_state(mqtt, g->id, "shell_opened", "shell connected");

    /* Start the idle-timeout monitor once per process. */
    pthread_mutex_lock(&s_monitor_lock);
    if (!s_monitor_started) {
        pthread_t tid;
        if (pthread_create(&tid, NULL, shell_monitor, NULL) == 0) {
            pthread_detach(tid);
            s_monitor_started = 1;
        }
    }
    pthread_mutex_unlock(&s_monitor_lock);
    return 0;
}

int shell_write(const char *guest_id, const char *data,
                char *errbuf, size_t errbuf_sz)
{
    /*
     * Take the fd under the lock, then write outside it.
     *
     * The write used to happen with s_lock held. write() to a pipe blocks once
     * the pipe fills, which is what happens whenever the guest stops draining
     * its stdin -- and holding the global lock through that froze every other
     * shell operation in the process, including the idle monitor that exists
     * to clean up exactly this situation. One wedged guest took the whole
     * shell subsystem with it.
     *
     * The fd may be closed by shell_close() while we are writing; that turns
     * the write into EBADF or EPIPE, which is reported like any other write
     * failure. SIGPIPE is ignored in main().
     */
    int fd = -1;
    int slot = -1;
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < SHELL_MAX_SESSIONS; i++) {
        if (s_sessions[i].alive && strcmp(s_sessions[i].guest_id, guest_id) == 0) {
            fd = s_sessions[i].in_fd;
            slot = i;
            break;
        }
    }
    pthread_mutex_unlock(&s_lock);

    if (slot < 0 || fd < 0) {
        snprintf(errbuf, errbuf_sz, "no open shell for this guest");
        return -1;
    }

    size_t len = strlen(data);
    size_t off = 0;
    int rc = 0, saved_errno = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            saved_errno = errno;
            rc = -1;
            break;
        }
        off += (size_t)w;
    }

    if (rc == 0) {
        pthread_mutex_lock(&s_lock);
        /* Re-check the slot still belongs to this guest: the reader thread may
           have released it while the write was in flight. */
        if (s_sessions[slot].alive &&
            strcmp(s_sessions[slot].guest_id, guest_id) == 0)
            s_sessions[slot].last_activity = time(NULL);
        pthread_mutex_unlock(&s_lock);
    } else {
        snprintf(errbuf, errbuf_sz, "write to shell failed: %s",
                 strerror(saved_errno));
    }
    return rc;
}

int shell_close(const char *guest_id)
{
    pthread_mutex_lock(&s_lock);
    ShellSession *s = NULL;
    for (int i = 0; i < SHELL_MAX_SESSIONS; i++) {
        if (s_sessions[i].alive && strcmp(s_sessions[i].guest_id, guest_id) == 0) {
            s = &s_sessions[i];
            break;
        }
    }
    if (!s) {
        pthread_mutex_unlock(&s_lock);
        return -1;
    }

    s->alive = 0;
    pid_t pid = s->pid;
    int in_fd = s->in_fd;
    s->pid = 0;
    s->in_fd = -1;
    pthread_mutex_unlock(&s_lock);

    /* Closing stdin makes ssh exit cleanly; TERM then KILL as a backstop. */
    if (in_fd >= 0) close(in_fd);
    if (pid > 0) {
        kill(pid, SIGTERM);
        for (int i = 0; i < 50; i++) {
            if (kill(pid, 0) != 0) break;
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 50000000L;
            nanosleep(&ts, NULL);
        }
        kill(pid, SIGKILL);
    }
    return 0;
}

int shell_is_open(const char *guest_id)
{
    int found = 0;
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < SHELL_MAX_SESSIONS; i++) {
        if (s_sessions[i].alive && strcmp(s_sessions[i].guest_id, guest_id) == 0) {
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&s_lock);
    return found;
}

void shell_close_all(void)
{
    for (int i = 0; i < SHELL_MAX_SESSIONS; i++) {
        pthread_mutex_lock(&s_lock);
        int alive = s_sessions[i].alive;
        char id[GUEST_ID_LEN];
        snprintf(id, sizeof(id), "%.31s", s_sessions[i].guest_id);
        pthread_mutex_unlock(&s_lock);
        if (alive && id[0])
            shell_close(id);
    }
}
