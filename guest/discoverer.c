#include "discoverer.h"
#include "address.h"
#include "../utils/proc.h"
#include "../ssh/client.h"
#include "lifecycle.h"
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

/*
 * Addresses resolved for guests whose .hms_metadata had none. Same reason as
 * the hostname cache above -- the Guest array is rebuilt from scratch twice a
 * second -- plus one of its own: resolving runs vpctl, and a guest whose
 * metadata file cannot be written would otherwise fork one every cycle forever.
 */
static struct {
    char id[GUEST_ID_LEN];
    char ip[GUEST_IP_LEN];
} ip_cache[MAX_GUESTS];

static const char *ip_cache_get(const char *id)
{
    for (int i = 0; i < MAX_GUESTS; i++)
        if (ip_cache[i].id[0] && strcmp(ip_cache[i].id, id) == 0)
            return ip_cache[i].ip;
    return NULL;
}

static void ip_cache_put(const char *id, const char *ip)
{
    for (int i = 0; i < MAX_GUESTS; i++) {
        if (!ip_cache[i].id[0]) {
            snprintf(ip_cache[i].id, sizeof(ip_cache[i].id), "%s", id);
            snprintf(ip_cache[i].ip, sizeof(ip_cache[i].ip), "%s", ip);
            return;
        }
    }
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

/*
 * Give the guest an address when .hms_metadata does not carry one, and write it
 * back so the file is the answer from here on.
 *
 * Adoption recorded pid= and stopped there, which left the more important half
 * missing: a guest started outside HMS was correctly reported RUNNING and then
 * could not be reached at all. Every ssh path -- the hostname fetch, OTA, any
 * command the GUI sends -- checks g->ip first and gives up on an empty one, so
 * the guest showed up as running and nameless, with nothing working on it.
 */
static void guest_fill_ip(Guest *g)
{
    const char *cached = ip_cache_get(g->id);
    if (cached) {
        snprintf(g->ip, sizeof(g->ip), "%s", cached);
        return;
    }

    char ip[GUEST_IP_LEN];
    if (guest_resolve_ip(g, ip, sizeof(ip)) != 0)
        return;

    snprintf(g->ip, sizeof(g->ip), "%s", ip);
    ip_cache_put(g->id, ip);
    guest_meta_set(g, "ip", ip);
    printf("  [hms] guest '%s' had no ip in .hms_metadata -- resolved %s\n",
           g->id, ip);
}

/* Callers must have checked the length (discover_guests() does, and reports
   it); assert it here too so the truncation is impossible rather than merely
   unlikely. */
static int discover_one(Guest *g, const char *dir_name)
{
    memset(g, 0, sizeof(*g));

    size_t id_len = strlen(dir_name);
    if (id_len >= sizeof(g->id))
        return -1;
    memcpy(g->id, dir_name, id_len + 1);
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
        /* The name qvm publishes itself under while this guest runs. Read here,
           once per discovery, because refresh_guest_state() needs it on every
           cycle and it cannot change while the guest is up. */
        guest_system_name(g, g->system_name, sizeof(g->system_name));
        /* HMS metadata (ip, ssh_*, pid) lives in its own .hms_metadata file,
           never in the guest's partition files. */
        if (access(meta_file, F_OK) == 0)
            parse_conf_network(meta_file,
                               g->ip, sizeof(g->ip),
                               g->ssh_user, sizeof(g->ssh_user),
                               g->ssh_password, sizeof(g->ssh_password),
                               g->ssh_key, sizeof(g->ssh_key),
                               &g->ssh_port, &g->pid);

        /* No ip= in the file, or no file: derive one and record it. Runs for a
           stopped guest too -- the host's vdevpeer links are bound at host boot
           and do not depend on the guest running, so the address is knowable
           before anything has started. */
        if (g->ip[0] == '\0')
            guest_fill_ip(g);
    }
    return 0;
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

        /* g->id is GUEST_ID_LEN and the name was copied into it with snprintf,
           so a longer directory name was silently truncated -- and the guest
           then half-worked: conf_path was built from the full name here, while
           kill, the metadata file and every OTA path were built from the
           truncated id and pointed at a directory that does not exist. Skip it
           and say so instead. */
        if (strlen(entry->d_name) >= GUEST_ID_LEN) {
            printf("  [hms] skipping '%s': name is longer than %d characters\n",
                   entry->d_name, GUEST_ID_LEN - 1);
            continue;
        }

        /* stat to verify it's a directory (QNX lacks d_type) */
        char full_path[GUEST_PATH_LEN + 16];
        snprintf(full_path, sizeof(full_path), "/guests/%s", entry->d_name);
        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        Guest *g = &out[count];
        if (discover_one(g, entry->d_name) != 0) continue;
        refresh_guest_state(g);
        count++;
    }
    closedir(d);
    return count;
}

/* Is `pid` a live qvm process? */
static int pid_is_qvm(int pid, char *cmdline, size_t cmd_sz)
{
    if (pid <= 0) return 0;

    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d", pid);
    if (access(proc_path, F_OK) != 0) return 0;

    char local[4096];
    char *buf = cmdline ? cmdline : local;
    size_t sz = cmdline ? cmd_sz : sizeof(local);

    if (!read_proc_cmdline(pid, buf, sz)) return 0;
    return strstr(buf, "qvm") != NULL;
}

/*
 * Does this qvm command line belong to this guest?
 *
 * guest_start() execs `qvm @<conf basename>` with cwd /guests/<id>, so the
 * directory is not on the command line and the basename is all there is. A
 * guest started by hand may equally have been launched with the full path.
 * Accept either, and accept the id itself for a launcher that names it.
 *
 * The basename alone is weak evidence -- two guests can both ship a file called
 * qnx-guest.qvmconf -- so the caller only trusts it when it matches exactly one
 * guest. That is what the match count in find_qvm_pid() below is for.
 */
static int cmdline_names_guest(const char *cmdline, const Guest *g)
{
    if (g->conf_path[0] && strstr(cmdline, g->conf_path))
        return 1;

    char dir[GUEST_PATH_LEN];
    snprintf(dir, sizeof(dir), "/guests/%s/", g->id);
    if (strstr(cmdline, dir))
        return 1;

    const char *base = strrchr(g->conf_path, '/');
    base = base ? base + 1 : g->conf_path;
    if (base[0] && strstr(cmdline, base))
        return 1;

    return 0;
}

/*
 * Find the live qvm process belonging to this guest by scanning /proc.
 * Returns its pid, or 0 if there is no unambiguous match.
 */
static int find_qvm_pid(const Guest *g)
{
    int pids[512];
    int n = scan_proc(pids, (int)(sizeof(pids) / sizeof(pids[0])));

    int found = 0, matches = 0;
    for (int i = 0; i < n; i++) {
        char cmdline[4096];
        if (!pid_is_qvm(pids[i], cmdline, sizeof(cmdline)))
            continue;
        if (!cmdline_names_guest(cmdline, g))
            continue;
        matches++;
        found = pids[i];
    }

    /* Two processes claiming the same guest means the evidence is the shared
       basename and not the guest. Report nothing rather than the wrong pid --
       a wrong pid here is a kill command aimed at another guest. */
    return matches == 1 ? found : 0;
}

/*
 * Is the guest running, regardless of who started it?
 *
 * qvm publishes /dev/qvm/<system> for as long as the guest lives, so this is
 * the one signal that does not depend on HMS having been the one to launch it.
 * It is also what the host's own vdevpeer setup already relies on:
 * /dev/qvm/guest_1/guest_to_host is where vpctl binds.
 */
static int qvm_dev_present(const Guest *g)
{
    if (!g->system_name[0]) return 0;

    char path[GUEST_PATH_LEN];
    snprintf(path, sizeof(path), "/dev/qvm/%s", g->system_name);

    struct stat st;
    return stat(path, &st) == 0;
}

/*
 * Which guests we have already announced as adopted, and as which pid.
 * discover_guests() rebuilds the Guest array from scratch every cycle -- twice
 * a second in the main loop -- so a flag in the struct cannot remember this and
 * the message would be printed forever.
 */
static struct {
    char id[GUEST_ID_LEN];
    int  pid;
} adopt_log[MAX_GUESTS];

static int adopt_announce(const char *id, int pid)
{
    for (int i = 0; i < MAX_GUESTS; i++) {
        if (adopt_log[i].id[0] && strcmp(adopt_log[i].id, id) == 0) {
            if (adopt_log[i].pid == pid) return 0;
            adopt_log[i].pid = pid;
            return 1;
        }
    }
    for (int i = 0; i < MAX_GUESTS; i++) {
        if (!adopt_log[i].id[0]) {
            snprintf(adopt_log[i].id, sizeof(adopt_log[i].id), "%s", id);
            adopt_log[i].pid = pid;
            return 1;
        }
    }
    return 0;
}

static void adopt_forget(const char *id)
{
    for (int i = 0; i < MAX_GUESTS; i++)
        if (adopt_log[i].id[0] && strcmp(adopt_log[i].id, id) == 0)
            adopt_log[i].id[0] = '\0';
}

/*
 * Work out whether the guest is running and, if so, which process it is.
 *
 * The old version asked one question -- "is the pid HMS wrote to .hms_metadata
 * still alive?" -- and so could only ever see guests HMS had started itself. A
 * guest launched from the console or a boot script had no pid recorded, was
 * reported STOPPED while plainly running, and the GUI offered Start for it:
 * pressing that ran a second qvm against a guest that already owned its vdevs.
 *
 * Three independent signals now, strongest first. Any one of them is enough to
 * call the guest running.
 */
void refresh_guest_state(Guest *g)
{
    g->state = GUEST_STOPPED;
    g->pid   = 0;

    /* 1. The pid HMS recorded, if it is still a live qvm. Cheapest, and the
          common case for a guest HMS started. */
    char meta[GUEST_PATH_LEN];
    snprintf(meta, sizeof(meta), "/guests/%s/.hms_metadata", g->id);

    int meta_pid = 0;
    parse_conf_network(meta, NULL, 0, NULL, 0, NULL, 0, NULL, 0, NULL, &meta_pid);

    if (meta_pid > 0 && pid_is_qvm(meta_pid, NULL, 0)) {
        g->pid   = meta_pid;
        g->state = GUEST_RUNNING;
        return;
    }

    /* 2. /dev/qvm/<system>, and 3. a qvm in /proc whose command line names this
          guest. Either alone establishes that it is running; together they also
          give the pid, which is what kill and restart need. */
    int dev_up = qvm_dev_present(g);

    /* The /proc scan is the expensive half -- every pid, every cmdline -- and
       this runs twice a second. Skip it for a guest /dev/qvm says is down,
       which is the steady state for a stopped guest. A guest whose qvmconf has
       no `system` line has no such answer, so it is scanned regardless. */
    int scan_pid = (dev_up || !g->system_name[0]) ? find_qvm_pid(g) : 0;

    if (!dev_up && scan_pid == 0) {
        adopt_forget(g->id);            /* genuinely stopped */
        return;
    }

    g->state = GUEST_RUNNING;
    g->pid   = scan_pid;                /* 0 if the scan could not name it */

    /* Adopt it: record the pid so kill/restart work on a guest HMS did not
       start, and so the next cycle takes the cheap path above. A guest we know
       is running but cannot name a process for keeps pid 0 -- reporting it as
       stopped would be worse, and writing a guessed pid far worse than that. */
    if (scan_pid > 0) {
        char pid_str[32];
        snprintf(pid_str, sizeof(pid_str), "%d", scan_pid);
        guest_meta_set(g, "pid", pid_str);
    }

    g->adopted = 1;
    if (adopt_announce(g->id, scan_pid)) {
        if (scan_pid > 0)
            printf("  [hms] guest '%s' was already running as PID %d "
                   "(started outside HMS) -- adopted\n", g->id, scan_pid);
        else
            printf("  [hms] guest '%s' is running: /dev/qvm/%s exists, but no "
                   "qvm process could be matched to it, so kill and restart "
                   "are unavailable for it\n", g->id, g->system_name);
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

    /* `uname -n` rather than `hostname`: the guest images carry the same
       toybox as the host, and it has no hostname applet, so this fetch always
       came back empty and every guest showed its name as "-". */
    char *out = ssh_exec(g, "uname -n");
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
