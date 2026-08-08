#include "guest/guest.h"
#include "guest/discoverer.h"
#include "guest/lifecycle.h"
#include "ssh/client.h"
#include "ssh/shell.h"
#include "config/config.h"
#include "mqtt/mqtt_client.h"
#include "ota/ota.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>

/*
 * `guests` is written by the refresh loop on the main thread and read by every
 * command handler, which runs on libmosquitto's network thread -- plus by the
 * OTA worker threads through the kill/start callbacks. It was shared with no
 * synchronisation at all: a "kill" arriving while refresh() was rebuilding the
 * array read a half-written Guest, and a Guest* handed to a handler could be
 * overwritten under it mid-command.
 *
 * The lock is only ever held around the array itself. refresh() does its slow
 * work (a /proc scan, and an SSH per guest for the hostname, each with a 5 s
 * connect timeout) into a local array and swaps it in under the lock, and
 * handlers copy the Guest they need out and release before doing anything with
 * it. Holding the lock across any of that would make one unreachable guest
 * stall every command for the length of its SSH timeout.
 */
static Guest           guests[MAX_GUESTS];
static int             guest_count = 0;
static pthread_mutex_t guests_lock = PTHREAD_MUTEX_INITIALIZER;
static HmsConfig       cfg;
static hms_mqtt_t      mqtt;
static volatile sig_atomic_t running = 1;

static void apply_defaults(Guest *g)
{
    if (g->ssh_port <= 0) g->ssh_port = cfg.ssh_default_port;
    if (g->ssh_user[0] == '\0')
        snprintf(g->ssh_user, sizeof(g->ssh_user), "%s", cfg.ssh_default_user);
    if (g->ssh_password[0] == '\0' && cfg.ssh_default_password[0] != '\0')
        snprintf(g->ssh_password, sizeof(g->ssh_password), "%s", cfg.ssh_default_password);
    if (g->ssh_key[0] == '\0' && cfg.ssh_key_path[0] != '\0')
        snprintf(g->ssh_key, sizeof(g->ssh_key), "%s", cfg.ssh_key_path);
}

static void refresh(void)
{
    /*
     * Heap, not stack. A Guest is ~1.7 KB and MAX_GUESTS of them is ~27 KB,
     * and refresh() is not only called from main: cmd_list/start/kill/info/
     * files all call it from libmosquitto's network thread, whose stack is
     * whatever the library asked for rather than the generous one the initial
     * thread gets. 27 KB of locals there is not worth the gamble on a target
     * where the failure mode is a stack overflow rather than a diagnostic.
     */
    Guest *scratch = malloc(MAX_GUESTS * sizeof(Guest));
    if (!scratch) {
        fprintf(stderr, "[hms] refresh: out of memory\n");
        return;
    }

    int n = discover_guests(scratch);
    for (int i = 0; i < n; i++) {
        apply_defaults(&scratch[i]);
        refresh_guest_name(&scratch[i]);
    }

    pthread_mutex_lock(&guests_lock);
    if (n > 0)
        memcpy(guests, scratch, (size_t)n * sizeof(Guest));
    guest_count = n;
    pthread_mutex_unlock(&guests_lock);

    free(scratch);
}

/* Copy the named guest out of the shared array. Returns 1 on success. */
static int get_guest(const char *id, Guest *out)
{
    int found = 0;
    pthread_mutex_lock(&guests_lock);
    Guest *g = find_guest(guests, guest_count, id);
    if (g) { *out = *g; found = 1; }
    pthread_mutex_unlock(&guests_lock);
    return found;
}

/*
 * Record a state transition an operation has just made, so the next command
 * does not act on the previous state in the window before refresh() runs.
 */
static void set_guest_state(const char *id, GuestState state, int pid)
{
    pthread_mutex_lock(&guests_lock);
    Guest *g = find_guest(guests, guest_count, id);
    if (g) { g->state = state; g->pid = pid; }
    pthread_mutex_unlock(&guests_lock);
}

static int escape_json(const char *in, char *out, size_t out_sz);

static void publish_result(const char *cmd, const char *guest_id,
                           int success, const char *msg)
{
    /* msg carries ssh/tar/scp output and file names, which routinely contain
       quotes, backslashes and newlines. Pasted in raw they produced a payload
       the GUI's JSON.parse() rejected, so the one message that would have said
       what went wrong was the one message that never arrived. */
    char esc[1024];
    escape_json(msg ? msg : "", esc, sizeof(esc));

    char buf[1400];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"result\",\"cmd\":\"%s\",\"guest\":\"%s\","
             "\"success\":%s,\"msg\":\"%s\"}",
             cmd, guest_id ? guest_id : "", success ? "true" : "false", esc);
    hms_mqtt_publish_status(&mqtt, buf);
}

/*
 * Look up a guest for a command, publishing the "unknown guest" result when it
 * is not there. Returns 1 and fills `out` on success.
 */
static int find_guest_or_publish(const char *cmd, const char *id, Guest *out)
{
    if (get_guest(id, out))
        return 1;
    char msg[128];
    snprintf(msg, sizeof(msg), "unknown guest '%.64s'", id ? id : "");
    publish_result(cmd, id, 0, msg);
    return 0;
}

/*
 * Append to a JSON buffer without ever running off the end.
 *
 * snprintf() returns the length it *would* have written, so the `pos +=
 * snprintf(...)` this file used everywhere could leave pos past the end of the
 * buffer -- and the next call's `sizeof(buf) - pos` then underflowed to an
 * enormous size_t, turning a truncated message into a heap overflow. Returns 0
 * when the text did not fit, so callers can stop early.
 */
static int json_append(char *buf, size_t sz, int *pos, const char *fmt, ...)
{
    if (*pos < 0 || (size_t)*pos >= sz)
        return 0;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *pos, sz - (size_t)*pos, fmt, ap);
    va_end(ap);

    if (n < 0) return 0;
    if ((size_t)n >= sz - (size_t)*pos) {
        buf[sz - 1] = '\0';          /* leave the buffer terminated */
        *pos = (int)sz;
        return 0;
    }
    *pos += n;
    return 1;
}

static void publish_guest_list_qos(int qos)
{
    char buf[8192];
    int pos = 0;

    pthread_mutex_lock(&guests_lock);
    json_append(buf, sizeof(buf), &pos, "{\"state\":\"guest_list\",\"guests\":[");
    for (int i = 0; i < guest_count; i++) {
        Guest *g = &guests[i];
        /* "running" is a fact about qvm, not about the guest being usable:
           qvm exists immediately, sshd inside the guest only once it has
           booted. Everything SSH-based fails with "Connection refused" in
           between, which reads as broken rather than as not-ready-yet.
           refresh_guest_name() already establishes reachability -- it fetches
           the hostname over SSH -- so reporting it costs nothing. */
        if (!json_append(buf, sizeof(buf), &pos,
                         "%s{\"id\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"state\":\"%s\","
                         "\"pid\":%d,\"ip\":\"%s\",\"reachable\":%s}",
                         i ? "," : "",
                         g->id,
                         g->name[0] ? g->name : "-",
                         guest_type_str(g->type),
                         guest_state_str(g->state),
                         g->pid,
                         g->ip[0] ? g->ip : "-",
                         g->name[0] ? "true" : "false"))
            break;
    }
    pthread_mutex_unlock(&guests_lock);

    /* Always close the array, even if a guest had to be dropped: a truncated
       payload is not JSON and the GUI drops the whole list rather than showing
       the guests that did fit. */
    if ((size_t)pos > sizeof(buf) - 3)
        pos = (int)sizeof(buf) - 3;
    snprintf(buf + pos, sizeof(buf) - (size_t)pos, "]}");
    hms_mqtt_publish_status_qos(&mqtt, buf, qos);
}

/* A reply to something the user did (list / start / kill): sent exactly once,
   so a dropped packet cannot leave a button looking like it did nothing. */
static void publish_guest_list(void)
{
    publish_guest_list_qos(HMS_MQTT_QOS);
}

static void publish_guest_info(const Guest *g)
{
    char buf[3072];
    snprintf(buf, sizeof(buf),
             "{"
             "\"state\":\"guest_info\","
             "\"guest\":{"
             "\"id\":\"%s\","
             "\"name\":\"%s\","
             "\"type\":\"%s\","
             "\"state\":\"%s\","
             "\"pid\":%d,"
             "\"ip\":\"%s\","
             "\"conf\":\"%s\","
             "\"boot\":\"%s\","
             "\"rootfs\":\"%s\","
             "\"ssh_user\":\"%s\","
             "\"ssh_port\":%d,"
             "\"ssh_key\":\"%s\""
             "}}",
             g->id, g->name,
             guest_type_str(g->type),
             guest_state_str(g->state),
             g->pid,
             g->ip[0] ? g->ip : "-",
             g->conf_path, g->boot_path, g->rootfs_path,
             g->ssh_user, g->ssh_port,
             g->ssh_key[0] ? g->ssh_key : "-");
    hms_mqtt_publish_status(&mqtt, buf);
}

static void cmd_list(void)
{
    refresh();
    publish_guest_list();
}

static void cmd_start(const char *id, const char *ip)
{
    Guest g;
    if (!find_guest_or_publish("start", id, &g)) return;

    if (ip && ip[0] != '\0') {
        snprintf(g.ip, sizeof(g.ip), "%s", ip);
        guest_set_ip(&g);
    }
    int rc = guest_start(&g);
    refresh();
    publish_result("start", id, rc == 0,
                   rc == 0 ? "guest started" : "failed to start guest");
    publish_guest_list();
}

static void cmd_kill(const char *id)
{
    Guest g;
    if (!find_guest_or_publish("kill", id, &g)) return;

    int rc = guest_kill(&g);
    refresh();
    publish_result("kill", id, rc == 0,
                   rc == 0 ? "guest killed" : "failed to kill guest");
    publish_guest_list();
}

static void cmd_info(const char *id)
{
    /* Refresh first, then look the guest up. The old order took the pointer,
       then called refresh() -- which rebuilds the array from scratch -- and
       then read through the pointer, so the details published belonged to
       whichever guest had landed at that index. */
    refresh();

    Guest g;
    if (!find_guest_or_publish("info", id, &g)) return;
    publish_guest_info(&g);
}

/*
 * JSON-escape a string in place into out (NUL-terminated).
 * Returns the number of characters written (excluding NUL).
 */
static int escape_json(const char *in, char *out, size_t out_sz)
{
    int j = 0;
    for (int i = 0; in[i] && j < (int)out_sz - 8; i++) {
        char c = in[i];
        if (c == '"' || c == '\\')      out[j++] = '\\', out[j++] = c;
        else if (c == '\n')             out[j++] = '\\', out[j++] = 'n';
        else if (c == '\r')             out[j++] = '\\', out[j++] = 'r';
        else if (c == '\t')             out[j++] = '\\', out[j++] = 't';
        else if ((unsigned char)c < 0x20) {
            int n = snprintf(out + j, out_sz - j, "\\u%04x", c);
            j += n;
        } else out[j++] = c;
    }
    out[j] = '\0';
    return j;
}

static void cmd_exec(const char *id, const char *command)
{
    Guest g;
    if (!find_guest_or_publish("exec", id, &g)) return;

    if (g.state != GUEST_RUNNING) {
        publish_result("exec", id, 0, "guest is not running");
        return;
    }

    /* `out` was reassigned to a string literal when ssh_exec() returned NULL
       and then free()d unconditionally at the end -- so every failed exec,
       which is exactly when a guest is unreachable, freed a pointer into
       read-only memory and took HMS down with it. */
    char ssherr[512];
    char *out = ssh_exec_diag(&g, command, ssherr, sizeof(ssherr));

    /* Say what actually went wrong. "(no output / SSH failed)" was the same
       string for a wrong key, an unreachable address and a command that simply
       printed nothing, so the Remote Shell could not be debugged from the GUI
       at all. */
    char failtext[600];
    const char *text;
    if (out) {
        text = out;
    } else {
        snprintf(failtext, sizeof(failtext), "ssh to %s failed: %s",
                 g.ip[0] ? g.ip : "(no address)",
                 ssherr[0] ? ssherr : "no output and no error text");
        text = failtext;
    }

    char escaped[4096];
    escape_json(text, escaped, sizeof(escaped));

    char buf[6144];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"exec_result\",\"guest\":\"%s\",\"output\":\"%s\"}",
             id, escaped);
    hms_mqtt_publish_status(&mqtt, buf);
    free(out);
}

/* Commands producing system stats (identical locally and inside a guest).
   Sections are delimited by echo markers so the output is trivial to parse.
   Each marker is preceded by a bare 'echo;' so it always starts on a fresh
   line even when the previous command omits its trailing newline
   (QNX 'hostname' does). QNX 8 notes: no 'uptime' binary (load comes from
   'pidin cpu'), RAM from 'pidin info' FreeMem, and per-process CPU time
   from 'ps -A' (plain 'pidin' prints a thread table without TIME). */
/* `uname -n`, not `hostname`.
 *
 * The host image links /bin/hostname to toybox, but this toybox is not built
 * with a hostname applet -- so the very first command of every stats poll
 * printed `toybox: Unknown command hostname` to the console and returned
 * nothing, several times a second while the Monitor tab was open. `uname -n`
 * gives the same node name, is in this toybox's applet list, and is POSIX, so
 * it works inside a Linux guest too. */
/*
 * One bundle that has to work on the QNX host, a QNX guest and a Linux guest.
 *
 * It was QNX-only: pidin does not exist on Linux, so every section of the
 * Linux guest's stats came back as "not found" and its Monitor tab was blank
 * with no explanation. Rather than branch on g->type -- which puts the
 * knowledge in the caller and does the wrong thing the moment a guest's type
 * is mislabelled in its metadata -- each section tries the QNX tool, then the
 * Linux one, then something universal, and takes whichever answers first.
 *
 * `pidin cpu` is gone. It is not a valid pidin shorthand ("invalid or
 * ambiguous shorthand"), so on QNX that section only ever produced an error,
 * and on Linux it produced nothing at all. `pidin times` is the real one.
 *
 * Every section is capped with head. Uncapped, a single QNX guest returned
 * 56 KB -- almost all of it the full per-thread process table -- which after
 * JSON escaping, doubled for host+guest, is close to a megabyte on the wire
 * every poll, for a page that displays a few dozen rows.
 */
static const char STATS_CMD[] =
    "echo '###HOSTNAME'; uname -n 2>/dev/null;"
    "echo '###KERNEL'; uname -a 2>/dev/null;"
    "echo '###UPTIMESEC'; (cat /proc/uptime 2>/dev/null || uptime 2>/dev/null) | head -2;"
    "echo '###CPUINFO'; (pidin info 2>/dev/null || cat /proc/cpuinfo 2>/dev/null"
        " || nproc 2>/dev/null) | head -20;"
    "echo '###CPUUSE'; (pidin times 2>/dev/null || cat /proc/stat 2>/dev/null)"
        " | head -12;"
    "echo '###MEM'; (pidin mem 2>/dev/null || free -m 2>/dev/null"
        " || cat /proc/meminfo 2>/dev/null) | head -25;"
    "echo '###PROC'; (top -b -i 1 2>/dev/null || top -b -n 1 2>/dev/null"
        " || ps aux 2>/dev/null || ps -A 2>/dev/null) | head -40;"
    "echo '###END'";

/* Capture the output of a local shell command. Caller must free(). */
static char *run_local(const char *command)
{
    FILE *fp = popen(command, "r");
    if (!fp) return NULL;

    size_t cap = 16384, len = 0;
    char *out = malloc(cap);
    if (!out) { pclose(fp); return NULL; }

    size_t n;
    while ((n = fread(out + len, 1, cap - len - 1, fp)) > 0) {
        len += n;
        if (cap - len - 1 < 1024) {
            cap *= 2;
            char *tmp = realloc(out, cap);
            if (!tmp) { free(out); pclose(fp); return NULL; }
            out = tmp;
        }
    }
    out[len] = '\0';
    pclose(fp);
    return out;
}

static void cmd_stats(const char *id)
{
    int running = 0;
    char guest_id[GUEST_ID_LEN] = "";
    Guest g;

    if (id && id[0]) {
        snprintf(guest_id, sizeof(guest_id), "%s", id);
        if (!get_guest(id, &g)) {
            publish_result("stats", id, 0, "unknown guest");
            return;
        }
        running = (g.state == GUEST_RUNNING);
    }

    char *host_esc = malloc(262144);
    if (!host_esc) return;

    /* Same literal-free() crash as cmd_exec(): host_out was replaced with a
       string literal on failure and free()d below. Keep the fallback in a
       separate const pointer so only a real allocation is ever freed. */
    char *host_out = run_local(STATS_CMD);
    escape_json(host_out ? host_out : "(failed to collect host stats)",
                host_esc, 262144);
    free(host_out);

    char *guest_esc = malloc(262144);
    if (!guest_esc) { free(host_esc); return; }
    guest_esc[0] = '\0';

    /* Why the guest half is missing, when it is. The Monitor page used to show
       an empty guest section with no explanation whenever the SSH failed --
       indistinguishable from the guest being stopped. */
    char guest_err[512] = "";
    if (running) {
        char *guest_out = ssh_exec_diag(&g, STATS_CMD, guest_err, sizeof(guest_err));
        if (guest_out) {
            escape_json(guest_out, guest_esc, 262144);
            free(guest_out);
        }
        /*
         * A failed stats fetch used to clear `running`, so the Monitor page
         * showed a red "stopped" badge for a guest that qvm says is very much
         * alive and that the Guests tab was listing as running at the same
         * moment. Two different questions were being answered with one field:
         * "is this guest running" (which qvm knows, locally and reliably) and
         * "could we collect stats from it" (which needs ssh and, on a guest
         * where a handshake alone costs 5-6s, fails often enough to matter).
         *
         * `running` now only ever reflects the first. guest_error carries the
         * second, and the GUI already prints it under the empty panel.
         */
    }

    char err_esc[1024];
    escape_json(guest_err, err_esc, sizeof(err_esc));

    char *buf = malloc(786432);
    if (buf) {
        snprintf(buf, 786432,
                 "{\"state\":\"monitor_stats\",\"guest_id\":\"%s\","
                 "\"guest_running\":%s,\"guest_error\":\"%s\","
                 "\"host\":\"%s\",\"guest\":\"%s\"}",
                 guest_id, running ? "true" : "false", err_esc,
                 host_esc, guest_esc);
        hms_mqtt_publish_status(&mqtt, buf);
        free(buf);
    }
    free(host_esc);
    free(guest_esc);
}

/* Called from an OTA worker thread: copy out, act, then record the transition
   so a following start callback does not see the pre-kill state. */
static int ota_kill_cb(const char *id)
{
    Guest g;
    if (!get_guest(id, &g) || g.state != GUEST_RUNNING) return 0;
    int rc = guest_kill(&g);
    set_guest_state(id, GUEST_STOPPED, 0);
    return rc;
}

static int ota_start_cb(const char *id)
{
    Guest g;
    if (!get_guest(id, &g)) return -1;
    int rc = guest_start(&g);
    if (rc == 0)
        set_guest_state(id, GUEST_RUNNING, 0); /* discoverer finds the real PID */
    return rc;
}

static void cmd_ota(const char *id, const char *remote_path)
{
    if (!id[0] || !remote_path[0]) {
        publish_result("ota", id, 0, "usage: ota <guest> <remote_path>");
        return;
    }
    Guest g;
    if (!get_guest(id, &g)) {
        publish_result("ota", id, 0, "unknown guest");
        return;
    }

    if (ota_start(&mqtt, &cfg, id, remote_path, ota_kill_cb, ota_start_cb) != 0)
        publish_result("ota", id, 0, "failed to start OTA job");
    else
        publish_result("ota", id, 1, "OTA job started");
}

static const char *base_only(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/*
 * Publish the list of partitions in /guests/<id>/ for one guest.
 * Only partition files are listed: the boot image (kind "ifs" or "img"),
 * rootfs.img / any other *.img (kind "img"), guest.conf and the qvmconf
 * (both kind "conf"). HMS-internal files (.hms_metadata, qvm.*) are never listed.
 */
static void publish_guest_files(const Guest *g)
{
    char buf[8192];
    int pos = 0, first = 1;

    json_append(buf, sizeof(buf), &pos,
                "{\"state\":\"guest_files\",\"guest\":\"%s\",\"type\":\"%s\","
                "\"running\":%s,\"files\":[",
                g->id, guest_type_str(g->type),
                (g->state == GUEST_RUNNING) ? "true" : "false");

    char dir[GUEST_PATH_LEN];
    snprintf(dir, sizeof(dir), "/guests/%s", g->id);

    char boot[GUEST_PATH_LEN] = "";
    guest_boot_image(g, boot, sizeof(boot));

    char qvmconf_name[GUEST_PATH_LEN];
    snprintf(qvmconf_name, sizeof(qvmconf_name), "%s",
             g->conf_path[0] ? base_only(g->conf_path) : "");

#define GF_ADD(name, kind, always) \
    do { \
        char fpath[GUEST_PATH_LEN * 2]; \
        snprintf(fpath, sizeof(fpath), "%s/%s", dir, (name)); \
        struct stat stt; \
        int ex = (stat(fpath, &stt) == 0); \
        long long sz = ex ? (long long)stt.st_size : -1; \
        if (ex || (always)) { \
            char jn[256], jk[64]; \
            escape_json((name), jn, sizeof(jn)); \
            escape_json((kind), jk, sizeof(jk)); \
            if (json_append(buf, sizeof(buf), &pos, \
                    "%s{\"name\":\"%s\",\"kind\":\"%s\",\"exists\":%s,\"size\":%lld}", \
                    first ? "" : ",", jn, jk, ex ? "true" : "false", sz)) \
                first = 0; \
        } \
    } while (0)

    if (boot[0])
        GF_ADD(boot, strstr(boot, ".ifs") ? "ifs" : "img", 1);
    GF_ADD("rootfs.img", "img", 0);
    GF_ADD("guest.conf", "conf", 0);
    if (qvmconf_name[0] && strstr(qvmconf_name, ".qvmconf"))
        GF_ADD(qvmconf_name, "conf", 0);

    /* Any other *.img (excluding the boot image and rootfs) is also an img. */
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            if (!strstr(e->d_name, ".img")) continue;
            if (boot[0] && strcmp(e->d_name, boot) == 0) continue;
            if (strcmp(e->d_name, "rootfs.img") == 0) continue;
            GF_ADD(e->d_name, "img", 0);
        }
        closedir(d);
    }

#undef GF_ADD
    if ((size_t)pos > sizeof(buf) - 3)
        pos = (int)sizeof(buf) - 3;
    snprintf(buf + pos, sizeof(buf) - (size_t)pos, "]}");
    hms_mqtt_publish_status(&mqtt, buf);
}

static void cmd_files(const char *id)
{
    if (!id || !id[0]) {
        publish_result("files", NULL, 0, "usage: files <guest>");
        return;
    }
    refresh();
    Guest g;
    if (!get_guest(id, &g)) {
        publish_result("files", id, 0, "unknown guest");
        return;
    }
    publish_guest_files(&g);
}

static void cmd_fetch(const char *id, char paths[][1024], int n_paths)
{
    Guest g;
    if (!find_guest_or_publish("fetch", id, &g)) return;

    for (int i = 0; i < n_paths; i++) {
        const char *name = base_only(paths[i]);
        if (strncmp(name, "qvm.", 4) == 0 || strcmp(name, ".hms_metadata") == 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "refusing to fetch runtime file '%.120s'", name);
            publish_result("fetch", id, 0, msg);
            return;
        }
    }

    if (ota_fetch_start(&mqtt, &cfg, id, n_paths, paths,
                        ota_kill_cb, ota_start_cb) != 0)
        publish_result("fetch", id, 0, "failed to start fetch job");
    else
        publish_result("fetch", id, 1, "fetch job started");
}

static void cmd_apply(const char *id, int restart)
{
    Guest g;
    if (!find_guest_or_publish("apply", id, &g)) return;

    if (ota_apply_start(&mqtt, &cfg, id, restart,
                        ota_kill_cb, ota_start_cb) != 0)
        publish_result("apply", id, 0, "failed to start apply job");
    else
        publish_result("apply", id, 1, "apply job started");
}

static void cmd_pushfiles(const char *id, const char *remote_path)
{
    Guest g;
    if (!find_guest_or_publish("pushfiles", id, &g)) return;

    if (g.state != GUEST_RUNNING) {
        publish_result("pushfiles", id, 0, "guest is not running");
        return;
    }

    if (ota_push_start(&mqtt, &cfg, &g, remote_path) != 0)
        publish_result("pushfiles", id, 0, "failed to start push job");
    else
        publish_result("pushfiles", id, 1, "push job started");
}

static void cmd_addfile(const char *id, const char *remote_path)
{
    Guest g;
    if (!find_guest_or_publish("addfile", id, &g)) return;

    if (ota_addfile_start(&mqtt, &cfg, id, remote_path) != 0)
        publish_result("addfile", id, 0, "failed to start add-file job");
    else
        publish_result("addfile", id, 1, "add-file job started");
}

static void cmd_addguest(const char *id, const char *ifs_remote,
                         const char *conf_remote, const char *ip)
{
    /* addguest is the one command that names a directory that does not exist
       yet, so nothing downstream can reject a bad name for us: the id goes
       straight into "mkdir -p /guests/<id>". Anything outside the discovery
       charset -- a '..', a ';', a '$(' -- would either escape /guests or run
       as a shell command on the host as root. */
    if (!guest_id_is_valid(id)) {
        publish_result("addguest", id, 0,
                       "invalid guest id (allowed: letters, digits, '.', '_', '-')");
        return;
    }

    if (ota_addguest_start(&mqtt, &cfg, id, ifs_remote, conf_remote, ip) != 0)
        publish_result("addguest", id, 0, "failed to start add-guest job");
    else
        publish_result("addguest", id, 1, "add-guest job started");
}

static void cmd_shellopen(const char *id)
{
    Guest g;
    if (!find_guest_or_publish("shellopen", id, &g)) return;

    if (g.state != GUEST_RUNNING) {
        publish_result("shellopen", id, 0, "guest is not running");
        return;
    }
    char errbuf[256];
    if (shell_open(&mqtt, &g, errbuf, sizeof(errbuf)) != 0)
        publish_result("shellopen", id, 0, errbuf);
    else
        publish_result("shellopen", id, 1, "shell opened");
}

static void cmd_shellwrite(const char *id, const char *data)
{
    char errbuf[256];
    if (shell_write(id, data, errbuf, sizeof(errbuf)) != 0)
        publish_result("shellwrite", id, 0, errbuf);
    /* On success nothing is published: the guest's answer streams back as
       shell_out messages. */
}

static void cmd_shellclose(const char *id)
{
    if (shell_close(id) != 0)
        publish_result("shellclose", id, 0, "no open shell for this guest");
    else
        publish_result("shellclose", id, 1, "shell closed");
}

static void dispatch_command(const char *cmd)
{
    printf("[hms] cmd: %s\n", cmd);

    /* tokenize: first token = action, rest = arguments.
       strtok keeps state in the library, so this must not run on two threads
       at once -- see the worker pool below, which serialises entry here. */
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", cmd);
    char *save = NULL;
    char *action = strtok_r(buf, " ", &save);
    if (!action) return;

    if (strcmp(action, "list") == 0) {
        cmd_list();
    }
    else if (strcmp(action, "start") == 0) {
        char *id = strtok_r(NULL, " ", &save);
        char *ip = strtok_r(NULL, " ", &save);
        if (!id) { publish_result("start", NULL, 0, "usage: start <guest> [ip]"); return; }
        cmd_start(id, ip ? ip : "");
    }
    else if (strcmp(action, "kill") == 0) {
        char *id = strtok_r(NULL, " ", &save);
        if (!id) { publish_result("kill", NULL, 0, "usage: kill <guest>"); return; }
        cmd_kill(id);
    }
    else if (strcmp(action, "info") == 0) {
        char *id = strtok_r(NULL, " ", &save);
        if (!id) { publish_result("info", NULL, 0, "usage: info <guest>"); return; }
        cmd_info(id);
    }
    else if (strcmp(action, "exec") == 0) {
        char *id = strtok_r(NULL, " ", &save);
        char *rest = strtok_r(NULL, "", &save);
        if (!id || !rest) { publish_result("exec", NULL, 0, "usage: exec <guest> <command>"); return; }
        cmd_exec(id, rest);
    }
    else if (strcmp(action, "stats") == 0) {
        char *id = strtok_r(NULL, " ", &save);
        cmd_stats(id ? id : "");
    }
    else if (strcmp(action, "ota") == 0) {
        char *id = strtok_r(NULL, " ", &save);
        char *rest = strtok_r(NULL, "", &save);
        if (!id || !rest) { publish_result("ota", NULL, 0, "usage: ota <guest> <remote_path>"); return; }
        cmd_ota(id, rest);
    }
    else if (strcmp(action, "files") == 0) {
        char *id = strtok_r(NULL, " ", &save);
        if (!id) { publish_result("files", NULL, 0, "usage: files <guest>"); return; }
        cmd_files(id);
    }
    else if (strcmp(action, "fetch") == 0) {
        char *id = strtok_r(NULL, " ", &save);
        if (!id) { publish_result("fetch", NULL, 0, "usage: fetch <guest> <file> [<file>...]"); return; }
        char paths[OTA_APPLY_MAX_FILES][1024];
        int n = 0, dropped = 0;
        char *t;
        while ((t = strtok_r(NULL, " ", &save)) != NULL) {
            if (n < OTA_APPLY_MAX_FILES)
                snprintf(paths[n++], sizeof(paths[0]), "%s", t);
            else
                dropped++;
        }
        if (n == 0) {
            publish_result("fetch", id, 0, "no files specified");
            return;
        }
        /* Over the cap, the extras used to be replaced by a literal
           "(truncated)" entry that was then passed on as a file to fetch --
           so the job failed on a file name that never existed. Fetch what
           fits and say what was left out. */
        if (dropped) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "too many files: fetching the first %d, %d ignored",
                     OTA_APPLY_MAX_FILES, dropped);
            publish_result("fetch", id, 0, msg);
        }
        cmd_fetch(id, paths, n);
    }
    else if (strcmp(action, "apply") == 0) {
        char *id = strtok_r(NULL, " ", &save);
        if (!id) { publish_result("apply", NULL, 0, "usage: apply <guest>"); return; }
        char *t = strtok_r(NULL, " ", &save);
        int restart = (t && strcmp(t, "--no-restart") == 0) ? 0 : 1;
        cmd_apply(id, restart);
    }
    else if (strcmp(action, "pushfiles") == 0) {
        char *id = strtok_r(NULL, " ", &save);
        if (!id) { publish_result("pushfiles", NULL, 0, "usage: pushfiles <guest> <serverPath>"); return; }
        char *path = strtok_r(NULL, " ", &save);
        if (!path) { publish_result("pushfiles", id, 0, "no server path specified"); return; }
        cmd_pushfiles(id, path);
    }
    else if (strcmp(action, "addfile") == 0) {
        char *id = strtok_r(NULL, " ", &save);
        if (!id) { publish_result("addfile", NULL, 0, "usage: addfile <guest> <serverPath>"); return; }
        char *path = strtok_r(NULL, " ", &save);
        if (!path) { publish_result("addfile", id, 0, "no server path specified"); return; }
        cmd_addfile(id, path);
    }
    else if (strcmp(action, "addguest") == 0) {
        char *id = strtok_r(NULL, " ", &save);
        char *ifs = strtok_r(NULL, " ", &save);
        char *conf = strtok_r(NULL, " ", &save);
        char *ip = strtok_r(NULL, " ", &save);
        if (!id || !ifs || !conf) {
            publish_result("addguest", NULL, 0, "usage: addguest <guest> <ifs_path> <conf_path> [ip]");
            return;
        }
        cmd_addguest(id, ifs, conf, ip ? ip : "");
    }
    else if (strcmp(action, "shellopen") == 0) {
        char *id = strtok_r(NULL, " ", &save);
        if (!id) { publish_result("shellopen", NULL, 0, "usage: shellopen <guest>"); return; }
        cmd_shellopen(id);
    }
    else if (strcmp(action, "shellwrite") == 0) {
        char *id = strtok_r(NULL, " ", &save);
        char *rest = strtok_r(NULL, "", &save);
        if (!id || !rest) { publish_result("shellwrite", NULL, 0, "usage: shellwrite <guest> <data>"); return; }
        cmd_shellwrite(id, rest);
    }
    else if (strcmp(action, "shellclose") == 0) {
        char *id = strtok_r(NULL, " ", &save);
        if (!id) { publish_result("shellclose", NULL, 0, "usage: shellclose <guest>"); return; }
        cmd_shellclose(id);
    }
    else if (strcmp(action, "ping") == 0) {
        hms_mqtt_publish_status(&mqtt, "{\"state\":\"pong\"}");
    }
    else {
        publish_result(action, NULL, 0, "unknown command");
    }
}

/* ===================== command queue and worker pool =====================
 *
 * handle_command() is called by libmosquitto on its single network thread --
 * the same thread that answers the broker's keepalive and receives every other
 * message. Running the work there meant one slow command stopped everything:
 *
 *   `stats` runs `top -b -i 1` locally and then SSHes into the guest, and an
 *   unreachable guest costs the full 5 s connect timeout. With the Monitor tab
 *   polling every 3 s, HMS spent essentially all of its time inside stats, and
 *   an `exec` typed into the Remote Shell sat behind it until the GUI's own
 *   15 s timeout gave up. Long blocks there also risk the broker dropping the
 *   session on keepalive.
 *
 * The network thread now only enqueues. Workers do the work, so a slow stats
 * no longer delays a shell command.
 */
#define CMD_QUEUE_MAX 32
#define CMD_WORKERS    4

static struct {
    char            items[CMD_QUEUE_MAX][2048];
    int             head, tail, count;
    int             stats_running;   /* see the coalescing note below */
    pthread_mutex_t lock;
    pthread_cond_t  cv;
} cmdq = {
    .head = 0, .tail = 0, .count = 0, .stats_running = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cv   = PTHREAD_COND_INITIALIZER,
};

static int is_stats_cmd(const char *c)
{
    return strncmp(c, "stats", 5) == 0 && (c[5] == '\0' || c[5] == ' ');
}

static void *cmd_worker(void *arg)
{
    (void)arg;
    for (;;) {
        char cmd[2048];

        pthread_mutex_lock(&cmdq.lock);
        while (cmdq.count == 0 && running)
            pthread_cond_wait(&cmdq.cv, &cmdq.lock);
        if (cmdq.count == 0 && !running) {
            pthread_mutex_unlock(&cmdq.lock);
            return NULL;
        }
        snprintf(cmd, sizeof(cmd), "%s", cmdq.items[cmdq.head]);
        cmdq.head = (cmdq.head + 1) % CMD_QUEUE_MAX;
        cmdq.count--;
        int mine_is_stats = is_stats_cmd(cmd);
        pthread_mutex_unlock(&cmdq.lock);

        dispatch_command(cmd);

        if (mine_is_stats) {
            pthread_mutex_lock(&cmdq.lock);
            cmdq.stats_running = 0;
            pthread_mutex_unlock(&cmdq.lock);
        }
    }
}

/* Called on libmosquitto's network thread. Must not block. */
static void handle_command(void *userdata, const char *cmd)
{
    (void)userdata;

    pthread_mutex_lock(&cmdq.lock);

    /*
     * Coalesce stats.
     *
     * The GUI polls every few seconds and re-sends when its own watchdog
     * fires, so when HMS is slower than the poll interval the requests pile
     * up -- which is exactly what the console showed, `cmd: stats` back to
     * back with no room for anything else. An extra stats while one is
     * already running answers a question that is about to be answered anyway,
     * so drop it rather than queue it.
     */
    /*
     * Queue space is checked BEFORE the stats flag is claimed, and this order
     * is load-bearing.
     *
     * It used to be the other way round: stats_running was set, and then the
     * full-queue check returned without ever clearing it. Only a worker clears
     * the flag, and a worker only runs commands that made it into the queue --
     * so a single stats arriving while the queue happened to be full left the
     * flag stuck at 1 for the lifetime of the process, and every stats after
     * that was silently dropped by the coalescing branch below. The Monitor
     * page then simply never received anything again, with HMS otherwise
     * healthy and answering everything else, which is exactly how it looked.
     */
    if (cmdq.count == CMD_QUEUE_MAX) {
        pthread_mutex_unlock(&cmdq.lock);
        fprintf(stderr, "[hms] command queue full, dropping: %.64s\n", cmd);
        return;
    }

    if (is_stats_cmd(cmd)) {
        if (cmdq.stats_running) {
            pthread_mutex_unlock(&cmdq.lock);
            return;
        }
        cmdq.stats_running = 1;
    }

    snprintf(cmdq.items[cmdq.tail], sizeof(cmdq.items[0]), "%s", cmd);
    cmdq.tail = (cmdq.tail + 1) % CMD_QUEUE_MAX;
    cmdq.count++;
    pthread_cond_signal(&cmdq.cv);
    pthread_mutex_unlock(&cmdq.lock);
}

static void on_signal(int sig)
{
    (void)sig;
    /* Only this. pthread_cond_broadcast() is not async-signal-safe, so waking
       the workers is left to main() once its loop has seen the flag. */
    running = 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("\n");
    printf("  Hypervisor Management System v0.2 (MQTT)\n");
    printf("  =========================================\n");

    signal(SIGCHLD, SIG_IGN);
    /* Ctrl-C used to leave every open shell's ssh child and the qvm children
       behind, and dropped the broker connection without a clean disconnect. */
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    /* A guest that closes its shell mid-write turns a write() to the ssh pipe
       into SIGPIPE, which by default kills HMS. shell_write() already reports
       the EPIPE. */
    signal(SIGPIPE, SIG_IGN);

    config_load(&cfg);
    refresh();

    /* Workers before the broker, so nothing arrives with nowhere to go. */
    pthread_t workers[CMD_WORKERS];
    int n_workers = 0;
    for (int i = 0; i < CMD_WORKERS; i++) {
        if (pthread_create(&workers[n_workers], NULL, cmd_worker, NULL) == 0)
            n_workers++;
    }
    if (n_workers == 0) {
        fprintf(stderr, "[hms] could not start any command workers\n");
        return 1;
    }
    printf("  [hms] %d command worker(s)\n", n_workers);

    if (hms_mqtt_init(&mqtt, handle_command, NULL) != 0) {
        fprintf(stderr, "[hms] MQTT init failed\n");
        return 1;
    }
    hms_mqtt_connect(&mqtt);

    /* Started here rather than on connect, so it exists across reconnects too
       -- it publishes only while connected and idles the rest of the time. */
    hms_mqtt_start_heartbeat(&mqtt);

    /* refresh_interval was read from hms.conf, stored, and then never looked
       at: the loop slept a hardcoded 2 s, so setting it did nothing. */
    int interval_s = cfg.refresh_interval_s > 0 ? cfg.refresh_interval_s : 2;
    printf("  [hms] refreshing guests every %d s\n", interval_s);

    /*
     * The full guest list goes out every cycle, unconditionally.
     *
     * This started as change detection -- fingerprint the list, publish only
     * when it differed -- which worked but was the wrong shape. Sending state
     * rather than events means there is no such thing as a missed edge: every
     * cycle the GUI is told everything, so a dropped packet, a GUI that
     * reconnects, a GUI started late, or a guest that changed during a
     * disconnect all correct themselves within one interval without a single
     * line of code handling any of those cases. The fingerprint needed a
     * special case for the disconnected state on its own.
     *
     * It costs a few hundred bytes every couple of seconds, at QoS 0. QoS 2
     * here would be a four-part handshake -- roughly 580ms of round trips on
     * the board's ~145ms link to the broker -- to guarantee delivery of a list
     * that is about to be sent again anyway. Losing one is free; the next one
     * supersedes it. Replies to list/start/kill stay at QoS 2, because a
     * dropped reply to something the user clicked is not self-correcting.
     *
     * Not folded into the heartbeat, though it beats twice as often: the data
     * only changes at this cadence, so half those sends would be identical
     * bytes, and the beat's payload would then depend on guests_lock -- which
     * would quietly make "is the board alive?" hostage to anyone who later
     * holds that lock too long.
     */
    while (running) {
        hms_mqtt_ensure_connected(&mqtt);
        refresh();
        publish_guest_list_qos(HMS_MQTT_QOS_BEAT);

        for (int i = 0; i < interval_s * 10 && running; i++) {
            /* nanosleep, not usleep: usleep was removed from POSIX.1-2008 and
               the build defines _POSIX_C_SOURCE=200809L, so it has no
               prototype here. */
            struct timespec ts = { 0, 100 * 1000 * 1000L };
            nanosleep(&ts, NULL);    /* interruptible by a signal */
        }
    }

    printf("\n[hms] shutting down\n");

    /* Wake the workers (the signal handler could not) and let them finish.
       A worker inside an SSH can be up to its 5 s connect timeout away from
       noticing, so this is not instant -- but it is bounded, and it is what
       stops us tearing down the MQTT client under a thread still publishing
       through it. */
    pthread_mutex_lock(&cmdq.lock);
    pthread_cond_broadcast(&cmdq.cv);
    pthread_mutex_unlock(&cmdq.lock);
    for (int i = 0; i < n_workers; i++)
        pthread_join(workers[i], NULL);

    shell_close_all();
    hms_mqtt_disconnect(&mqtt);
    return 0;
}
