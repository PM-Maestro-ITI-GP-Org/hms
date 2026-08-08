#include "client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>

/*
 * Is `cmd` on the PATH? The answer cannot change while HMS runs, so it is
 * looked up once and remembered in *cache (-1 unknown, 0 no, 1 yes).
 *
 * This used to be a fresh fork+exec of a shell writing to a /tmp file on
 * *every* ssh_exec() -- which the discoverer calls once per guest per refresh
 * cycle. Worse, the sequence counter naming that file was a plain static++
 * touched from three thread contexts at once, so two callers could pick the
 * same name and read back each other's answer.
 */
static int have_cmd(const char *cmd, int *cache)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

    pthread_mutex_lock(&lock);
    if (*cache < 0) {
        char probe[128];
        snprintf(probe, sizeof(probe), "command -v %s 2>/dev/null", cmd);
        *cache = 0;
        FILE *fp = popen(probe, "r");
        if (fp) {
            char line[256];
            if (fgets(line, sizeof(line), fp) && line[0] == '/')
                *cache = 1;
            pclose(fp);
        }
    }
    int r = *cache;
    pthread_mutex_unlock(&lock);
    return r;
}

static int have_sshpass(void) { static int c = -1; return have_cmd("sshpass", &c); }
static int have_timeout(void) { static int c = -1; return have_cmd("timeout", &c); }

/*
 * A wall-clock cap on a single ssh command.
 *
 * ConnectTimeout only bounds the TCP connect. Everything after it -- key
 * exchange, auth, the command itself -- is unbounded, so an ssh to a guest
 * that accepts the connection and then stops answering hangs forever. That is
 * not hypothetical: it wedged HMS on the board. Five `ssh ... uname -n`
 * processes were left REPLY-blocked, and because refresh() and all four
 * command workers each call through here, every one of them ended up stuck in
 * popen() and HMS stopped answering MQTT entirely while still looking alive.
 *
 * 20s: longer than any command HMS issues (the slowest is the stats bundle),
 * and shorter than the GUI's own 15s command timeout is useful beyond.
 *
 * ponytail: fixed cap for every exec; make it a per-call argument if a
 * legitimately long-running `exec` from the shell ever needs more.
 */
#define SSH_EXEC_TIMEOUT "20"

/* Check if a file exists */
static int file_exists(const char *path)
{
    return (access(path, F_OK) == 0);
}

/* Build the shared SSH options string (host key handling + port).
   scp uses uppercase -P for the port (lowercase -p means "preserve
   times" and would treat the port number as a local file!). */
void ssh_build_opts(const Guest *g, char *opts, size_t sz, int for_scp)
{
    int n = snprintf(opts, sz,
        /* accept-new, not no -- and no UserKnownHostsFile=/dev/null.
         *
         * Those two together meant every connection accepted whatever
         * answered and then threw the evidence away, so a guest could be
         * impersonated by anything that got onto the wire and nothing would
         * ever notice. They were there because guests regenerated their host
         * key on every boot, which made real checking impossible.
         *
         * They no longer do: the QNX guest ships a key generated at build
         * time and the host ships it in known_hosts, so that one is verified
         * from the very first connection. accept-new covers the guest that
         * still generates its own -- it pins on first contact and refuses a
         * change after, without ever prompting. */
        "-o StrictHostKeyChecking=accept-new "
        "-o LogLevel=ERROR "
        "-o ConnectTimeout=5 "
        "%s %d",
        for_scp ? "-P" : "-p",
        g->ssh_port);
    if (n < 0 || (size_t)n >= sz) return;
    if (g->ssh_key[0] != '\0' && file_exists(g->ssh_key)) {
        snprintf(opts + n, sz - (size_t)n, " -i %s", g->ssh_key);
    }
}

int ssh_scp_to(const Guest *g, const char *local_path, const char *remote_path,
               char *errbuf, size_t errbuf_sz)
{
    if (errbuf && errbuf_sz > 0)
        errbuf[0] = '\0';

    if (g->ip[0] == '\0') {
        printf("  [ssh] ERROR: guest '%s' has no IP address\n", g->id);
        if (errbuf) snprintf(errbuf, errbuf_sz, "guest has no IP address");
        return -1;
    }
    if (g->state != GUEST_RUNNING) {
        printf("  [ssh] ERROR: guest '%s' is not running\n", g->id);
        if (errbuf) snprintf(errbuf, errbuf_sz, "guest is not running");
        return -1;
    }
    if (!file_exists(local_path)) {
        printf("  [ssh] ERROR: local file missing: %s\n", local_path);
        if (errbuf) snprintf(errbuf, errbuf_sz, "local file missing");
        return -1;
    }

    /* Stream the file through plain ssh ("cat > dest") instead of scp:
       scp defaults to the SFTP subsystem, which QNX guests often don't
       have configured; plain ssh is the same proven path as ssh_exec. */
    char ssh_opts[1024];
    ssh_build_opts(g, ssh_opts, sizeof(ssh_opts), 0);

    char cmd[4096];
    int n;
    if (g->ssh_password[0] != '\0' && have_sshpass()) {
        n = snprintf(cmd, sizeof(cmd),
            "sshpass -p '%s' ssh %s %s@%s \"cat > %s\" < \"%s\" 2>&1",
            g->ssh_password, ssh_opts, g->ssh_user, g->ip, remote_path, local_path);
    } else if (g->ssh_key[0] != '\0' && file_exists(g->ssh_key)) {
        n = snprintf(cmd, sizeof(cmd),
            "ssh -o BatchMode=yes %s %s@%s \"cat > %s\" < \"%s\" 2>&1",
            ssh_opts, g->ssh_user, g->ip, remote_path, local_path);
    } else {
        n = snprintf(cmd, sizeof(cmd),
            "ssh %s %s@%s \"cat > %s\" < \"%s\" 2>&1",
            ssh_opts, g->ssh_user, g->ip, remote_path, local_path);
    }

    if (n >= (int)sizeof(cmd)) {
        printf("  [ssh] ERROR: push command too long\n");
        if (errbuf) snprintf(errbuf, errbuf_sz, "push command too long");
        return -1;
    }

    printf("  [ssh] push %s -> %s@%s:%s\n", local_path, g->ssh_user, g->ip, remote_path);
    printf("  [ssh] push cmd: %s\n", cmd);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        printf("  [ssh] ERROR: popen failed\n");
        if (errbuf) snprintf(errbuf, errbuf_sz, "popen failed");
        return -1;
    }

    /* Capture stdout+stderr; keep the tail so a failure can be diagnosed. */
    char buf[1024];
    char tail[4096] = "";
    while (fgets(buf, sizeof(buf), fp)) {
        size_t blen = strlen(buf);
        size_t tlen = strlen(tail);
        if (tlen + blen < sizeof(tail) - 1) {
            if (tlen + blen > sizeof(tail) - 256)
                memmove(tail, tail + blen, tlen + 1 - blen);
            strcat(tail, buf);
        }
    }
    (void)pclose(fp);

    /* QNX pclose() reports -1 even when the child exited 0, so its return
       value cannot be trusted. Judge success by the captured output (a real
       ssh failure always prints something to stderr) and by verifying the
       file size on the guest. */
    int failed = 0;
    if (strstr(tail, "Permission denied") || strstr(tail, "Connection refused")
        || strstr(tail, "Connection timed out") || strstr(tail, "No route to host")
        || strstr(tail, "not found") || strstr(tail, "No such file")
        || strstr(tail, "ERROR")) {
        failed = 1;
    }

    long long remote_sz = -1, local_sz = -1;
    if (!failed) {
        struct stat st;
        if (stat(local_path, &st) == 0)
            local_sz = (long long)st.st_size;
        char check[512];
        snprintf(check, sizeof(check), "ls -l %s", remote_path);
        char *out = ssh_exec(g, check);
        if (out) {
            char *tok = strtok(out, " \t\r\n");
            int idx = 0;
            while (tok) {
                if (idx == 4)
                    remote_sz = strtoll(tok, NULL, 10);
                tok = strtok(NULL, " \t\r\n");
                idx++;
            }
            free(out);
        }
        if (local_sz < 0 || remote_sz != local_sz)
            failed = 1;
    }

    if (failed) {
        printf("  [ssh] push to guest failed: %s", tail);
        if (errbuf) {
            if (remote_sz >= 0 && local_sz >= 0 && remote_sz != local_sz)
                snprintf(errbuf, errbuf_sz,
                         "size mismatch: guest has %lld bytes, expected %lld",
                         remote_sz, local_sz);
            else
                snprintf(errbuf, errbuf_sz, "ssh failed%s%s",
                         tail[0] ? ": " : "", tail);
        }
    }
    return failed ? -1 : 0;
}

/* Unique-per-call scratch name for capturing a command's stderr. */
static void tmp_name(char *out, size_t sz, const char *tag)
{
    static unsigned int seq = 0;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    unsigned int mine = seq++;
    pthread_mutex_unlock(&lock);
    snprintf(out, sz, "/tmp/hms_%s_%d_%u", tag, (int)getpid(), mine);
}

char *ssh_exec_diag(const Guest *g, const char *command,
                    char *errbuf, size_t errbuf_sz)
{
    if (errbuf && errbuf_sz) errbuf[0] = '\0';

    if (g->ip[0] == '\0') {
        printf("  [ssh] ERROR: guest '%s' has no IP address\n", g->id);
        if (errbuf) snprintf(errbuf, errbuf_sz, "guest has no IP address");
        return NULL;
    }
    if (g->state != GUEST_RUNNING) {
        printf("  [ssh] ERROR: guest '%s' is not running\n", g->id);
        if (errbuf) snprintf(errbuf, errbuf_sz, "guest is not running");
        return NULL;
    }

    /* Build SSH options */
    char ssh_opts[1024];
    ssh_build_opts(g, ssh_opts, sizeof(ssh_opts), 0);

    /*
     * stderr goes to a file rather than to HMS's own console.
     *
     * It used to be discarded, which is why every failure looked identical:
     * ssh printed "Permission denied (publickey)" or "No route to host" onto
     * the host's terminal, stdout came back empty, and the caller had nothing
     * to report but "(no output / SSH failed)". Keeping it separate from
     * stdout matters -- callers parse stdout (the guest browser parses `ls`),
     * so 2>&1 would corrupt it.
     */
    char errfile[128];
    tmp_name(errfile, sizeof(errfile), "ssherr");

    /* -k 5: SIGTERM at the cap, SIGKILL five seconds later if ssh ignores it. */
    const char *tmo = have_timeout() ? "timeout -k 5 " SSH_EXEC_TIMEOUT " " : "";

    /* The exit status, via a file. QNX's pclose() returns -1 even for a child
       that exited 0, so it cannot be used -- and without the status a command
       killed by `timeout` is indistinguishable from one that succeeded and
       printed nothing. `timeout` exits 124 when it fires. */
    char rcfile[128];
    tmp_name(rcfile, sizeof(rcfile), "sshrc");

    char cmd[4096];
    int n;
    if (g->ssh_password[0] != '\0' && have_sshpass()) {
        /* Password auth via sshpass */
        n = snprintf(cmd, sizeof(cmd),
            "%ssshpass -p '%s' ssh %s %s@%s \"%s\" 2>%s; echo $? >%s",
            tmo, g->ssh_password, ssh_opts, g->ssh_user, g->ip, command, errfile, rcfile);
    } else if (g->ssh_key[0] != '\0' && file_exists(g->ssh_key)) {
        /* Key-based auth with explicit identity */
        n = snprintf(cmd, sizeof(cmd),
            "%sssh -o BatchMode=yes %s %s@%s \"%s\" 2>%s; echo $? >%s",
            tmo, ssh_opts, g->ssh_user, g->ip, command, errfile, rcfile);
    } else if (g->ssh_password[0] != '\0') {
        /* Password configured but no sshpass and no key */
        printf("  [ssh] WARNING: password set but 'sshpass' not found and no SSH key available.\n");
        n = snprintf(cmd, sizeof(cmd),
            "%sssh -o BatchMode=yes %s %s@%s \"%s\" 2>%s; echo $? >%s",
            tmo, ssh_opts, g->ssh_user, g->ip, command, errfile, rcfile);
    } else {
        /* Default: no password, no key — rely on ssh-agent */
        n = snprintf(cmd, sizeof(cmd),
            "%sssh %s %s@%s \"%s\" 2>%s; echo $? >%s",
            tmo, ssh_opts, g->ssh_user, g->ip, command, errfile, rcfile);
    }

    if (n >= (int)sizeof(cmd)) {
        printf("  [ssh] ERROR: command too long\n");
        if (errbuf) snprintf(errbuf, errbuf_sz, "ssh command too long");
        return NULL;
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        printf("  [ssh] ERROR: popen failed\n");
        if (errbuf) snprintf(errbuf, errbuf_sz, "popen failed");
        remove(errfile);
        return NULL;
    }

    size_t cap = 4096, len = 0;
    char *out = malloc(cap);
    if (!out) { pclose(fp); remove(errfile); return NULL; }
    out[0] = '\0';

    char buf[1024];
    while (fgets(buf, sizeof(buf), fp)) {
        size_t blen = strlen(buf);
        if (len + blen + 1 > cap) {
            cap *= 2;
            char *tmp = realloc(out, cap);
            if (!tmp) { free(out); pclose(fp); remove(errfile); return NULL; }
            out = tmp;
        }
        memcpy(out + len, buf, blen + 1);
        len += blen;
    }
    (void)pclose(fp);

    /* Read back whatever ssh complained about. */
    char err[512] = "";
    FILE *ef = fopen(errfile, "r");
    if (ef) {
        size_t got = fread(err, 1, sizeof(err) - 1, ef);
        err[got] = '\0';
        fclose(ef);
    }
    remove(errfile);

    int rc = -1;
    FILE *rf = fopen(rcfile, "r");
    if (rf) {
        if (fscanf(rf, "%d", &rc) != 1) rc = -1;
        fclose(rf);
    }
    remove(rcfile);

    /* `timeout` exits 124. Without this the kill looks like a command that
       succeeded and printed nothing -- which would mark an unreachable guest
       as reachable and hide the very failure the cap exists to surface. */
    if (rc == 124) {
        snprintf(err, sizeof(err),
                 "no response within %ss (ssh was killed)", SSH_EXEC_TIMEOUT);
        printf("  [ssh] '%s' on %s timed out after %ss\n",
               command, g->id, SSH_EXEC_TIMEOUT);
        if (errbuf) snprintf(errbuf, errbuf_sz, "%s", err);
        free(out);
        return NULL;
    }

    /* Trim trailing newlines so the text sits on one line in the GUI. */
    size_t elen = strlen(err);
    while (elen > 0 && (err[elen - 1] == '\n' || err[elen - 1] == '\r'))
        err[--elen] = '\0';
    for (char *p = err; *p; p++)
        if (*p == '\n' || *p == '\r') *p = ' ';

    /*
     * Judge the result by output and stderr, not by pclose().
     *
     * The old test was `pclose(fp) != 0 && len == 0`. QNX's pclose reports -1
     * even for a child that exited 0 -- this file already says so where
     * ssh_scp_to explains the same problem -- so that condition reduced to
     * "no output", and every command that legitimately prints nothing (touch,
     * mkdir, an empty directory listing) was reported as a failed SSH.
     *
     * Empty output with a silent stderr is now a success, which is what it is.
     */
    if (len == 0 && err[0]) {
        printf("  [ssh] '%s' on %s failed: %s\n", command, g->id, err);
        if (errbuf) snprintf(errbuf, errbuf_sz, "%s", err);
        free(out);
        return NULL;
    }

    /* Succeeded, but ssh still had something to say (a host-key notice, a
       warning). Pass it up without failing the call. */
    if (err[0] && errbuf)
        snprintf(errbuf, errbuf_sz, "%s", err);

    return out;
}

char *ssh_exec(const Guest *g, const char *command)
{
    return ssh_exec_diag(g, command, NULL, 0);
}

int ssh_ping(const Guest *g)
{
    char *out = ssh_exec(g, "echo pong");
    if (!out) return 0;
    int ok = (strstr(out, "pong") != NULL);
    free(out);
    return ok;
}
