#include "lifecycle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

static void msleep(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* Set/update one key=value line in /guests/<id>/.hms_metadata, preserving all
   other keys. Writes atomically via a temp file + rename.

   Not static any more: the discoverer writes pid= too, when it adopts a guest
   that was started outside HMS. One writer for the file, whoever calls it. */
void guest_meta_set(const Guest *g, const char *key, const char *val)
{
    char path[GUEST_PATH_LEN];
    snprintf(path, sizeof(path), "/guests/%s/.hms_metadata", g->id);

    char lines[64][256];
    int n = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        while (n < 64 && fgets(lines[n], sizeof(lines[n]), f))
            n++;
        fclose(f);
    }

    char tmp[GUEST_PATH_LEN + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *out = fopen(tmp, "w");
    if (!out) {
        printf("  [hms] WARNING: could not write %s\n", tmp);
        return;
    }

    int wrote = 0;
    size_t klen = strlen(key);
    for (int i = 0; i < n; i++) {
        char *p = lines[i];
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            fprintf(out, "%s=%s\n", key, val);
            wrote = 1;
        } else {
            fputs(lines[i], out);
        }
    }
    if (!wrote)
        fprintf(out, "%s=%s\n", key, val);
    fclose(out);
    rename(tmp, path);
}

int guest_start(const Guest *g)
{
    if (g->state == GUEST_RUNNING) {
        printf("  [hms] guest '%s' is already running (PID %d)\n", g->id, g->pid);
        return 0;
    }

    if (access(g->conf_path, F_OK) != 0) {
        printf("  [hms] ERROR: config not found: %s\n", g->conf_path);
        return -1;
    }

    printf("  [hms] starting '%s' ...\n", g->id);

    pid_t pid = fork();
    if (pid == -1) {
        printf("  [hms] ERROR: fork failed (%s)\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* Child process — fully detach from parent's terminal */

        /* Create new session + process group to lose controlling terminal */
        setsid();

        /* Change to guest directory */
        char cwd[GUEST_PATH_LEN];
        snprintf(cwd, sizeof(cwd), "/guests/%s", g->id);
        if (chdir(cwd) != 0)
            _exit(1);

        /* Stdin from /dev/null; qvm output goes to a per-guest log so
           startup failures are diagnosable instead of lost. */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0)
            dup2(devnull, STDIN_FILENO);

        char log_path[GUEST_PATH_LEN];
        snprintf(log_path, sizeof(log_path), "/guests/%s/qvm.log", g->id);
        int logfd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logfd < 0) logfd = devnull;
        dup2(logfd, STDOUT_FILENO);
        dup2(logfd, STDERR_FILENO);
        if (devnull > STDERR_FILENO) close(devnull);
        if (logfd > STDERR_FILENO) close(logfd);

        /* Launch the discovered config by name (qvm resolves it from cwd) */
        const char *base = strrchr(g->conf_path, '/');
        char conf_arg[GUEST_PATH_LEN];
        snprintf(conf_arg, sizeof(conf_arg), "@%.*s",
                 (int)sizeof(conf_arg) - 2,
                 base ? base + 1 : g->conf_path);

        execl("/sbin/qvm", "qvm", conf_arg, NULL);

        /* If exec returns, it failed */
        fprintf(stderr, "[hms] ERROR: exec /sbin/qvm failed: %s\n", strerror(errno));
        _exit(2);
    }

    /* Parent — don't wait; child runs independently */
    printf("  [hms] '%s' launched as PID %d (config %s)\n", g->id, pid, g->conf_path);
    printf("  [hms] qvm output logged to /guests/%s/qvm.log\n", g->id);

    /* Store the PID in .hms_metadata for the discoverer to find later */
    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d", (int)pid);
    guest_meta_set(g, "pid", pid_str);

    /* Clean up any legacy qvm.pid file from older builds */
    char pidfile[GUEST_PATH_LEN];
    snprintf(pidfile, sizeof(pidfile), "/guests/%s/qvm.pid", g->id);
    remove(pidfile);

    /* Give QVM a moment to initialise; warn if it died right away */
    msleep(500);
    if (kill(pid, 0) != 0) {
        printf("  [hms] WARNING: '%s' exited immediately — check /guests/%s/qvm.log\n",
               g->id, g->id);
        return -1;
    }
    return 0;
}

int guest_kill(const Guest *g)
{
    if (g->state != GUEST_RUNNING) {
        printf("  [hms] guest '%s' is not running\n", g->id);
        return -1;
    }

    /* Running, but no process to signal. That is the adopted case where
       /dev/qvm/<system> proves the guest is up and no qvm command line could be
       matched to it -- say so, rather than "not running", which is the one
       thing it definitely is. */
    if (g->pid <= 0) {
        printf("  [hms] guest '%s' is running but HMS has no PID for it "
               "(started outside HMS and its qvm could not be identified); "
               "kill it from the console with 'slay qvm'\n", g->id);
        return -1;
    }

    printf("  [hms] killing '%s' (PID %d) ...\n", g->id, g->pid);

    /* Send SIGKILL directly — no shell invocation */
    int ret = kill(g->pid, SIGKILL);

    if (ret != 0) {
        printf("  [hms] ERROR: kill(%d, SIGKILL) failed (%s)\n",
               g->pid, strerror(errno));
        return -1;
    }

    /* Clear the stored PID (metadata file keeps ip=/ssh_* settings) */
    guest_meta_set(g, "pid", "0");

    /* Remove legacy PID file */
    char pidfile[GUEST_PATH_LEN];
    snprintf(pidfile, sizeof(pidfile), "/guests/%s/qvm.pid", g->id);
    remove(pidfile);

    msleep(200);
    return 0;
}

void guest_set_ip(const Guest *g)
{
    char meta[GUEST_PATH_LEN];
    snprintf(meta, sizeof(meta), "/guests/%s/.hms_metadata", g->id);

    /* Skip if the IP is already stored.
       The check was strstr(line, g->ip), a substring match anywhere in the
       line: setting 10.0.0.2 on a guest whose file already said ip=10.0.0.22
       matched, so the write was skipped and the address silently stayed
       wrong. Compare the value itself. */
    FILE *f = fopen(meta, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (p[0] == '#') p++;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "ip=", 3) != 0) continue;

            char *v = p + 3;
            while (*v == ' ' || *v == '\t') v++;
            char *end = v + strlen(v);
            while (end > v && (end[-1] == '\n' || end[-1] == '\r' ||
                               end[-1] == ' '  || end[-1] == '\t'))
                end--;
            *end = '\0';

            if (strcmp(v, g->ip) == 0) {
                fclose(f);
                printf("  [hms] IP %s already in %s\n", g->ip, meta);
                return;
            }
        }
        fclose(f);
    }

    guest_meta_set(g, "ip", g->ip);
    printf("  [hms] IP %s saved to %s\n", g->ip, meta);
}

int guest_restart(const Guest *g)
{
    guest_kill(g);
    msleep(300);
    return guest_start(g);
}
