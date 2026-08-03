#include "discoverer.h"
#include "../utils/proc.h"
#include "../ssh/client.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

/* Strip leading whitespace and trailing newline in-place. */
static char *trim_line(char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
    return line;
}

/*
 * Hostname cache keyed by guest id. discover_guests() rebuilds the array
 * from scratch every refresh cycle, so a fetched hostname would be lost;
 * keep it here instead and only evict when the guest stops.
 */
static struct {
    char id[GUEST_ID_LEN];
    char name[GUEST_NAME_LEN];
} name_cache[MAX_GUESTS];

static const char *name_cache_get(const char *id)
{
    for (int i = 0; i < MAX_GUESTS; i++)
        if (name_cache[i].id[0] && strcmp(name_cache[i].id, id) == 0)
            return name_cache[i].name[0] ? name_cache[i].name : NULL;
    return NULL;
}

static void name_cache_put(const char *id, const char *name)
{
    for (int i = 0; i < MAX_GUESTS; i++) {
        if (name_cache[i].id[0] && strcmp(name_cache[i].id, id) == 0) {
            if (name)
                snprintf(name_cache[i].name, sizeof(name_cache[i].name), "%s", name);
            else
                name_cache[i].id[0] = '\0'; /* evict */
            return;
        }
    }
    for (int i = 0; i < MAX_GUESTS; i++) {
        if (!name_cache[i].id[0]) {
            snprintf(name_cache[i].id, sizeof(name_cache[i].id), "%s", id);
            if (name)
                snprintf(name_cache[i].name, sizeof(name_cache[i].name), "%s", name);
            return;
        }
    }
}

/*
 * Scan the .hms_metadata file for HMS metadata keys (ip, ssh_user,
 * ssh_password, ssh_port, ssh_key, pid). Plain key=value lines; only writes
 * fields it finds and only into non-NULL outputs.
 */
static void parse_conf_network(const char *conf_path,
                               char *ip_out, size_t ip_sz,
                               char *user_out, size_t user_sz,
                               char *pass_out, size_t pass_sz,
                               char *key_out, size_t key_sz,
                               int *port_out, int *pid_out)
{
    FILE *f = fopen(conf_path, "r");
    if (!f) return;

    char line[256];

    while (fgets(line, sizeof(line), f)) {
        char *val = trim_line(line);
        if (val[0] == '\0') continue;

        /* Also accept HMS keys after a '#' so QVM doesn't reject them */
        if (val[0] == '#') val = trim_line(val + 1);

        if (strncmp(val, "ip=", 3) == 0) {
            char *v = val + 3; while (*v == ' ' || *v == '\t') v++;
            if (ip_out && ip_sz) { strncpy(ip_out, v, ip_sz - 1); ip_out[ip_sz - 1] = '\0'; }
        }
        else if (strncmp(val, "ssh_user=", 9) == 0) {
            char *v = val + 9; while (*v == ' ' || *v == '\t') v++;
            if (user_out && user_sz) { strncpy(user_out, v, user_sz - 1); user_out[user_sz - 1] = '\0'; }
        }
        else if (strncmp(val, "ssh_password=", 13) == 0) {
            char *v = val + 13; while (*v == ' ' || *v == '\t') v++;
            if (pass_out && pass_sz) { strncpy(pass_out, v, pass_sz - 1); pass_out[pass_sz - 1] = '\0'; }
        }
        else if (strncmp(val, "ssh_port=", 9) == 0) {
            if (port_out) *port_out = atoi(val + 9);
        }
        else if (strncmp(val, "ssh_key=", 8) == 0) {
            char *v = val + 8; while (*v == ' ' || *v == '\t') v++;
            if (key_out && key_sz) { strncpy(key_out, v, key_sz - 1); key_out[key_sz - 1] = '\0'; }
        }
        else if (strncmp(val, "pid=", 4) == 0) {
            if (pid_out) *pid_out = atoi(val + 4);
        }
    }
    fclose(f);
}

/* Concatenate two path components into out with hard bounds; always NUL-terminates. */
static void path_join(char *out, size_t out_sz, const char *a, const char *b)
{
    size_t na = a ? strlen(a) : 0;
    size_t nb = b ? strlen(b) : 0;
    if (na >= out_sz) na = out_sz - 1;
    memcpy(out, a, na);
    out[na] = '\0';
    if (na + 1 < out_sz) {
        size_t avail = out_sz - na - 1;
        if (nb > avail) nb = avail;
        memcpy(out + na, b, nb);
        out[na + nb] = '\0';
    }
}

static void discover_one(Guest *g, const char *dir_name)
{
    memset(g, 0, sizeof(*g));

    snprintf(g->id,   sizeof(g->id),   "%s", dir_name);
    g->name[0] = '\0';
    g->name_ts = 0;
    g->state = GUEST_STOPPED;
    g->pid   = 0;
    g->ssh_port = 22;
    strncpy(g->ssh_user, "root", sizeof(g->ssh_user) - 1);

    /* Build standard paths */
    char dir_path[GUEST_PATH_LEN];
    char meta_file[GUEST_PATH_LEN];
    path_join(dir_path,   sizeof(dir_path),   "/guests/", dir_name);
    path_join(meta_file,  sizeof(meta_file),  dir_path, "/.hms_metadata");
    path_join(g->conf_path, sizeof(g->conf_path), dir_path, "/guest.conf");
    path_join(g->boot_path, sizeof(g->boot_path), dir_path, "/boot.img");
    path_join(g->rootfs_path, sizeof(g->rootfs_path), dir_path, "/rootfs.img");

    /* The launch config: prefer a real QVM config (*.qvmconf). guest.conf
       is only an HMS metadata file (ip=, ssh_*) that QVM cannot parse. */
    DIR *d = opendir(dir_path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strstr(e->d_name, ".qvmconf") != NULL) {
                char dir_slash[GUEST_PATH_LEN];
                path_join(dir_slash, sizeof(dir_slash), dir_path, "/");
                path_join(g->conf_path, sizeof(g->conf_path), dir_slash, e->d_name);
                break;
            }
        }
        closedir(d);
    }

    if (access(g->conf_path, F_OK) == 0) {
        g->type = guest_type_from_conf(g->conf_path);
        /* Resolve the real boot image: QNX guests boot an IFS (qnx-guest.ifs /
           *.ifs), Linux/Android use boot.img. discover_one() had hardcoded
           boot.img above; correct it now that the type is known. */
        char boot[GUEST_PATH_LEN];
        if (guest_boot_image(g, boot, sizeof(boot)) == 0)
            snprintf(g->boot_path, sizeof(g->boot_path), "%s", boot);
        /* HMS metadata (ip, ssh_*, pid) lives in its own .hms_metadata file,
           never in the guest's partition files. */
        if (access(meta_file, F_OK) == 0)
            parse_conf_network(meta_file,
                               g->ip, sizeof(g->ip),
                               g->ssh_user, sizeof(g->ssh_user),
                               g->ssh_password, sizeof(g->ssh_password),
                               g->ssh_key, sizeof(g->ssh_key),
                               &g->ssh_port, &g->pid);
    }
}

int discover_guests(Guest out[MAX_GUESTS])
{
    int count = 0;

    DIR *d = opendir("/guests");
    if (!d) return 0;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && count < MAX_GUESTS) {
        if (entry->d_name[0] == '.') continue;
        if (strncmp(entry->d_name, "guest-", 6) != 0) continue;

        /* stat to verify it's a directory (QNX lacks d_type) */
        char full_path[GUEST_PATH_LEN];
        snprintf(full_path, sizeof(full_path), "/guests/%s", entry->d_name);
        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        Guest *g = &out[count];
        discover_one(g, entry->d_name);
        refresh_guest_state(g);
        count++;
    }
    closedir(d);
    return count;
}

void refresh_guest_state(Guest *g)
{
    g->state = GUEST_STOPPED;
    g->pid   = 0;

    /* Read the PID stored by guest_start() in .hms_metadata */
    char meta[GUEST_PATH_LEN];
    snprintf(meta, sizeof(meta), "/guests/%s/.hms_metadata", g->id);

    int pid = 0;
    parse_conf_network(meta, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, &pid);
    if (pid <= 0) return;

    /* Verify the process is still alive and is qvm; the metadata file itself
       is kept (it also holds ip=/ssh_* settings), only the state is cleared. */
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d", pid);
    if (access(proc_path, F_OK) != 0)
        return;

    char cmdline[4096];
    if (!read_proc_cmdline((int)pid, cmdline, sizeof(cmdline)))
        return;

    if (strstr(cmdline, "qvm") != NULL) {
        g->pid   = (int)pid;
        g->state = GUEST_RUNNING;
    }
}

void refresh_guest_name(Guest *g)
{
    const char *cached = name_cache_get(g->id);

    if (g->state != GUEST_RUNNING || g->ip[0] == '\0') {
        /* While stopped the name is unknown; evict and refetch after a restart. */
        if (cached)
            name_cache_put(g->id, NULL);
        g->name[0] = '\0';
        g->name_ts = 0;
        return;
    }

    /* Already fetched in a previous cycle: restore from the cache. */
    if (cached) {
        strncpy(g->name, cached, sizeof(g->name) - 1);
        g->name[sizeof(g->name) - 1] = '\0';
        return;
    }

    /* Throttle SSH attempts: one per 10 s while unreachable. */
    time_t now = time(NULL);
    if (now - g->name_ts < 10)
        return;
    g->name_ts = now;

    char *out = ssh_exec(g, "hostname");
    if (!out)
        return;

    /* Trim trailing whitespace / newline */
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' ||
                       out[len - 1] == ' ' || out[len - 1] == '\t'))
        out[--len] = '\0';

    if (len > 0 && len < sizeof(g->name))
        strncpy(g->name, out, len);
    g->name[len < sizeof(g->name) ? len : sizeof(g->name) - 1] = '\0';
    free(out);

    if (g->name[0] != '\0') {
        name_cache_put(g->id, g->name);
        printf("  [hms] guest '%s' hostname: %s\n", g->id, g->name);
    }
}

Guest *find_guest(Guest list[MAX_GUESTS], int count, const char *id)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(list[i].id, id) == 0)
            return &list[i];
    }
    return NULL;
}
