#include "client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* Check if a command is available on the host PATH */
static int cmd_exists(const char *cmd)
{
    char rcfile[128];
    static unsigned int seq = 0;
    snprintf(rcfile, sizeof(rcfile), "/tmp/ssh_rc_%d_%u", getpid(), seq++);
    char buf[512];
    snprintf(buf, sizeof(buf),
             "command -v %s >/dev/null 2>&1; echo $? > %s", cmd, rcfile);
    system(buf);
    int rc = -1;
    FILE *f = fopen(rcfile, "r");
    if (f) {
        if (fscanf(f, "%d", &rc) != 1) rc = -1;
        fclose(f);
        remove(rcfile);
    }
    return (rc == 0);
}

/* Check if a file exists */
static int file_exists(const char *path)
{
    return (access(path, F_OK) == 0);
}

/* Build the shared SSH options string (host key handling + port).
   scp uses uppercase -P for the port (lowercase -p means "preserve
   times" and would treat the port number as a local file!). */
static void build_ssh_opts(const Guest *g, char *opts, size_t sz, int for_scp)
{
    int n = snprintf(opts, sz,
        "-o StrictHostKeyChecking=no "
        "-o UserKnownHostsFile=/dev/null "
        "-o LogLevel=ERROR "
        "-o ConnectTimeout=5 "
        "%s %d",
        for_scp ? "-P" : "-p",
        g->ssh_port);
    if (g->ssh_key[0] != '\0' && file_exists(g->ssh_key)) {
        snprintf(opts + n, sz - n, " -i %s", g->ssh_key);
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
    build_ssh_opts(g, ssh_opts, sizeof(ssh_opts), 0);

    char cmd[4096];
    int n;
    if (g->ssh_password[0] != '\0' && cmd_exists("sshpass")) {
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

char *ssh_exec(const Guest *g, const char *command)
{
    if (g->ip[0] == '\0') {
        printf("  [ssh] ERROR: guest '%s' has no IP address\n", g->id);
        return NULL;
    }
    if (g->state != GUEST_RUNNING) {
        printf("  [ssh] ERROR: guest '%s' is not running\n", g->id);
        return NULL;
    }

    /* Build SSH options */
    char ssh_opts[1024];
    build_ssh_opts(g, ssh_opts, sizeof(ssh_opts), 0);

    char cmd[4096];
    int n;
    if (g->ssh_password[0] != '\0' && cmd_exists("sshpass")) {
        /* Password auth via sshpass */
        n = snprintf(cmd, sizeof(cmd),
            "sshpass -p '%s' ssh %s %s@%s \"%s\"",
            g->ssh_password, ssh_opts, g->ssh_user, g->ip, command);
    } else if (g->ssh_key[0] != '\0' && file_exists(g->ssh_key)) {
        /* Key-based auth with explicit identity */
        n = snprintf(cmd, sizeof(cmd),
            "ssh -o BatchMode=yes %s %s@%s \"%s\"",
            ssh_opts, g->ssh_user, g->ip, command);
    } else if (g->ssh_password[0] != '\0') {
        /* Password configured but no sshpass and no key */
        printf("  [ssh] WARNING: password set but 'sshpass' not found and no SSH key available.\n");
        n = snprintf(cmd, sizeof(cmd),
            "ssh -o BatchMode=yes %s %s@%s \"%s\"",
            ssh_opts, g->ssh_user, g->ip, command);
    } else {
        /* Default: no password, no key — rely on ssh-agent */
        n = snprintf(cmd, sizeof(cmd),
            "ssh %s %s@%s \"%s\"",
            ssh_opts, g->ssh_user, g->ip, command);
    }

    if (n >= (int)sizeof(cmd)) {
        printf("  [ssh] ERROR: command too long\n");
        return NULL;
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        printf("  [ssh] ERROR: popen failed\n");
        return NULL;
    }

    size_t cap = 4096, len = 0;
    char *out = malloc(cap);
    if (!out) { pclose(fp); return NULL; }
    out[0] = '\0';

    char buf[1024];
    while (fgets(buf, sizeof(buf), fp)) {
        size_t blen = strlen(buf);
        if (len + blen + 1 > cap) {
            cap *= 2;
            char *tmp = realloc(out, cap);
            if (!tmp) { free(out); pclose(fp); return NULL; }
            out = tmp;
        }
        memcpy(out + len, buf, blen + 1);
        len += blen;
    }

    int ret = pclose(fp);
    if (ret != 0 && len == 0) {
        free(out);
        return NULL;
    }
    return out;
}

int ssh_ping(const Guest *g)
{
    char *out = ssh_exec(g, "echo pong");
    if (!out) return 0;
    int ok = (strstr(out, "pong") != NULL);
    free(out);
    return ok;
}
