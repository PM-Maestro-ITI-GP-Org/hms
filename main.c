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
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

static Guest      guests[MAX_GUESTS];
static int        guest_count = 0;
static HmsConfig  cfg;
static hms_mqtt_t mqtt;

static void apply_defaults(Guest *g)
{
    if (g->ssh_port <= 0) g->ssh_port = cfg.ssh_default_port;
    if (g->ssh_user[0] == '\0')
        strncpy(g->ssh_user, cfg.ssh_default_user, sizeof(g->ssh_user));
    if (g->ssh_password[0] == '\0' && cfg.ssh_default_password[0] != '\0')
        strncpy(g->ssh_password, cfg.ssh_default_password, sizeof(g->ssh_password));
    if (g->ssh_key[0] == '\0' && cfg.ssh_key_path[0] != '\0')
        strncpy(g->ssh_key, cfg.ssh_key_path, sizeof(g->ssh_key));
}

static void refresh(void)
{
    guest_count = discover_guests(guests);
    for (int i = 0; i < guest_count; i++) {
        apply_defaults(&guests[i]);
        refresh_guest_name(&guests[i]);
    }
}

static Guest *find_guest_or_publish(const char *id)
{
    Guest *g = find_guest(guests, guest_count, id);
    if (!g) {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "{\"state\":\"result\",\"cmd\":\"%s\",\"guest\":\"%s\","
                 "\"success\":false,\"msg\":\"unknown guest '%s'\"}",
                 "command", id, id);
        hms_mqtt_publish_status(&mqtt, buf);
    }
    return g;
}

static void publish_result(const char *cmd, const char *guest_id,
                           int success, const char *msg)
{
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"result\",\"cmd\":\"%s\",\"guest\":\"%s\","
             "\"success\":%s,\"msg\":\"%s\"}",
             cmd, guest_id ? guest_id : "", success ? "true" : "false", msg);
    hms_mqtt_publish_status(&mqtt, buf);
}

static void publish_guest_list(void)
{
    char buf[8192];
    int pos = snprintf(buf, sizeof(buf), "{\"state\":\"guest_list\",\"guests\":[");
    for (int i = 0; i < guest_count && pos < (int)sizeof(buf) - 256; i++) {
        Guest *g = &guests[i];
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%s{\"id\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"state\":\"%s\","
                        "\"pid\":%d,\"ip\":\"%s\"}",
                        i ? "," : "",
                        g->id,
                        g->name[0] ? g->name : "-",
                        guest_type_str(g->type),
                        guest_state_str(g->state),
                        g->pid,
                        g->ip[0] ? g->ip : "-");
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    hms_mqtt_publish_status(&mqtt, buf);
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
    Guest *g = find_guest_or_publish(id);
    if (!g) return;

    if (ip && ip[0] != '\0') {
        snprintf(g->ip, sizeof(g->ip), "%s", ip);
        guest_set_ip(g);
    }
    int rc = guest_start(g);
    refresh();
    publish_result("start", id, rc == 0,
                   rc == 0 ? "guest started" : "failed to start guest");
    publish_guest_list();
}

static void cmd_kill(const char *id)
{
    Guest *g = find_guest_or_publish(id);
    if (!g) return;

    int rc = guest_kill(g);
    refresh();
    publish_result("kill", id, rc == 0,
                   rc == 0 ? "guest killed" : "failed to kill guest");
    publish_guest_list();
}

static void cmd_info(const char *id)
{
    Guest *g = find_guest_or_publish(id);
    if (!g) return;
    refresh();
    publish_guest_info(g);
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
    Guest *g = find_guest_or_publish(id);
    if (!g) return;

    if (g->state != GUEST_RUNNING) {
        publish_result("exec", id, 0, "guest is not running");
        return;
    }

    char *out = ssh_exec(g, command);
    if (!out) out = "(no output / SSH failed)";

    char escaped[4096];
    escape_json(out, escaped, sizeof(escaped));

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
static const char STATS_CMD[] =
    "echo; echo '###HOSTNAME'; hostname;"
    "echo; echo '###KERNEL'; uname -a;"
    "echo; echo '###UPTIMESEC'; cat /proc/uptime 2>/dev/null;"
    "echo; echo '###CPUINFO'; pidin info;"
    "echo; echo '###CPUUSE'; pidin cpu 2>/dev/null;"
    "echo; echo '###MEM'; pidin mem;"
    "echo; echo '###PROC'; top -b -i 1;"
    "echo; echo '###END'";

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

    if (id && id[0]) {
        snprintf(guest_id, sizeof(guest_id), "%s", id);
        Guest *g = find_guest(guests, guest_count, id);
        if (!g) {
            publish_result("stats", id, 0, "unknown guest");
            return;
        }
        running = (g->state == GUEST_RUNNING);
    }

    char *host_esc = malloc(262144);
    if (!host_esc) return;
    char *host_out = run_local(STATS_CMD);
    if (!host_out) host_out = "(failed to collect host stats)";
    escape_json(host_out, host_esc, 262144);

    char *guest_esc = malloc(262144);
    if (!guest_esc) { free(host_out); free(host_esc); return; }
    guest_esc[0] = '\0';
    if (running) {
        Guest *g = find_guest(guests, guest_count, id);
        char *guest_out = ssh_exec(g, STATS_CMD);
        if (guest_out) {
            escape_json(guest_out, guest_esc, 262144);
            free(guest_out);
        } else {
            running = 0;
        }
    }

    char *buf = malloc(786432);
    if (buf) {
        snprintf(buf, 786432,
                 "{\"state\":\"monitor_stats\",\"guest_id\":\"%s\","
                 "\"guest_running\":%s,"
                 "\"host\":\"%s\",\"guest\":\"%s\"}",
                 guest_id, running ? "true" : "false", host_esc, guest_esc);
        hms_mqtt_publish_status(&mqtt, buf);
        free(buf);
    }
    free(host_out);
    free(host_esc);
    free(guest_esc);
}

static int ota_kill_cb(const char *id)
{
    Guest *g = find_guest(guests, guest_count, id);
    if (!g || g->state != GUEST_RUNNING) return 0;
    int rc = guest_kill(g);
    /* Update the in-memory state so a subsequent start callback does not
     * think the guest is still running and skip the relaunch. */
    g->state = GUEST_STOPPED;
    g->pid = 0;
    return rc;
}

static int ota_start_cb(const char *id)
{
    Guest *g = find_guest(guests, guest_count, id);
    if (!g) return -1;
    int rc = guest_start(g);
    if (rc == 0) {
        g->state = GUEST_RUNNING;
        g->pid = 0; /* discoverer will pick up the real PID */
    }
    return rc;
}

static void cmd_ota(const char *id, const char *remote_path)
{
    if (!id[0] || !remote_path[0]) {
        publish_result("ota", id, 0, "usage: ota <guest> <remote_path>");
        return;
    }
    Guest *g = find_guest(guests, guest_count, id);
    if (!g) {
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
    int pos = 0, first = 1, n;

    n = snprintf(buf + pos, sizeof(buf) - pos,
                 "{\"state\":\"guest_files\",\"guest\":\"%s\",\"type\":\"%s\","
                 "\"running\":%s,\"files\":[",
                 g->id, guest_type_str(g->type),
                 (g->state == GUEST_RUNNING) ? "true" : "false");
    pos += n;

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
            pos += snprintf(buf + pos, sizeof(buf) - pos, \
                "%s{\"name\":\"%s\",\"kind\":\"%s\",\"exists\":%s,\"size\":%lld}", \
                first ? "" : ",", jn, jk, ex ? "true" : "false", sz); \
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
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    hms_mqtt_publish_status(&mqtt, buf);
}

static void cmd_files(const char *id)
{
    if (!id || !id[0]) {
        publish_result("files", NULL, 0, "usage: files <guest>");
        return;
    }
    refresh();
    Guest *g = find_guest(guests, guest_count, id);
    if (!g) {
        publish_result("files", id, 0, "unknown guest");
        return;
    }
    publish_guest_files(g);
}

static void cmd_fetch(const char *id, char paths[][1024], int n_paths)
{
    Guest *g = find_guest_or_publish(id);
    if (!g) return;

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
    Guest *g = find_guest_or_publish(id);
    if (!g) return;

    if (ota_apply_start(&mqtt, &cfg, id, restart,
                        ota_kill_cb, ota_start_cb) != 0)
        publish_result("apply", id, 0, "failed to start apply job");
    else
        publish_result("apply", id, 1, "apply job started");
}

static void cmd_pushfiles(const char *id, const char *remote_path)
{
    Guest *g = find_guest_or_publish(id);
    if (!g) return;

    if (g->state != GUEST_RUNNING) {
        publish_result("pushfiles", id, 0, "guest is not running");
        return;
    }

    if (ota_push_start(&mqtt, &cfg, g, remote_path) != 0)
        publish_result("pushfiles", id, 0, "failed to start push job");
    else
        publish_result("pushfiles", id, 1, "push job started");
}

static void cmd_addfile(const char *id, const char *remote_path)
{
    Guest *g = find_guest_or_publish(id);
    if (!g) return;

    if (ota_addfile_start(&mqtt, &cfg, id, remote_path) != 0)
        publish_result("addfile", id, 0, "failed to start add-file job");
    else
        publish_result("addfile", id, 1, "add-file job started");
}

static void cmd_addguest(const char *id, const char *ifs_remote,
                         const char *conf_remote, const char *ip)
{
    if (ota_addguest_start(&mqtt, &cfg, id, ifs_remote, conf_remote, ip) != 0)
        publish_result("addguest", id, 0, "failed to start add-guest job");
    else
        publish_result("addguest", id, 1, "add-guest job started");
}

static void cmd_shellopen(const char *id)
{
    Guest *g = find_guest_or_publish(id);
    if (!g) return;

    if (g->state != GUEST_RUNNING) {
        publish_result("shellopen", id, 0, "guest is not running");
        return;
    }
    char errbuf[256];
    if (shell_open(&mqtt, g, errbuf, sizeof(errbuf)) != 0)
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

static void handle_command(void *userdata, const char *cmd)
{
    (void)userdata;
    printf("[hms] cmd: %s\n", cmd);

    /* tokenize: first token = action, rest = arguments */
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", cmd);
    char *action = strtok(buf, " ");
    if (!action) return;

    if (strcmp(action, "list") == 0) {
        cmd_list();
    }
    else if (strcmp(action, "start") == 0) {
        char *id = strtok(NULL, " ");
        char *ip = strtok(NULL, " ");
        if (!id) { publish_result("start", NULL, 0, "usage: start <guest> [ip]"); return; }
        cmd_start(id, ip ? ip : "");
    }
    else if (strcmp(action, "kill") == 0) {
        char *id = strtok(NULL, " ");
        if (!id) { publish_result("kill", NULL, 0, "usage: kill <guest>"); return; }
        cmd_kill(id);
    }
    else if (strcmp(action, "info") == 0) {
        char *id = strtok(NULL, " ");
        if (!id) { publish_result("info", NULL, 0, "usage: info <guest>"); return; }
        cmd_info(id);
    }
    else if (strcmp(action, "exec") == 0) {
        char *id = strtok(NULL, " ");
        char *rest = strtok(NULL, "");
        if (!id || !rest) { publish_result("exec", NULL, 0, "usage: exec <guest> <command>"); return; }
        cmd_exec(id, rest);
    }
    else if (strcmp(action, "stats") == 0) {
        char *id = strtok(NULL, " ");
        cmd_stats(id ? id : "");
    }
    else if (strcmp(action, "ota") == 0) {
        char *id = strtok(NULL, " ");
        char *rest = strtok(NULL, "");
        if (!id || !rest) { publish_result("ota", NULL, 0, "usage: ota <guest> <remote_path>"); return; }
        cmd_ota(id, rest);
    }
    else if (strcmp(action, "files") == 0) {
        char *id = strtok(NULL, " ");
        if (!id) { publish_result("files", NULL, 0, "usage: files <guest>"); return; }
        cmd_files(id);
    }
    else if (strcmp(action, "fetch") == 0) {
        char *id = strtok(NULL, " ");
        if (!id) { publish_result("fetch", NULL, 0, "usage: fetch <guest> <file> [<file>...]"); return; }
        char paths[OTA_APPLY_MAX_FILES][1024];
        int n = 0, truncated = 0;
        char *t;
        while ((t = strtok(NULL, " ")) != NULL) {
            if (n < OTA_APPLY_MAX_FILES - 1) {
                snprintf(paths[n], sizeof(paths[n]), "%s", t);
                n++;
            } else {
                truncated = 1;   /* drop extras beyond the cap */
            }
        }
        if (truncated)
            snprintf(paths[OTA_APPLY_MAX_FILES - 1], sizeof(paths[0]), "(truncated)");
        if (n == 0 && !truncated) {
            publish_result("fetch", id, 0, "no files specified");
            return;
        }
        cmd_fetch(id, paths, truncated ? OTA_APPLY_MAX_FILES : n);
    }
    else if (strcmp(action, "apply") == 0) {
        char *id = strtok(NULL, " ");
        if (!id) { publish_result("apply", NULL, 0, "usage: apply <guest>"); return; }
        char *t = strtok(NULL, " ");
        int restart = (t && strcmp(t, "--no-restart") == 0) ? 0 : 1;
        cmd_apply(id, restart);
    }
    else if (strcmp(action, "pushfiles") == 0) {
        char *id = strtok(NULL, " ");
        if (!id) { publish_result("pushfiles", NULL, 0, "usage: pushfiles <guest> <serverPath>"); return; }
        char *path = strtok(NULL, " ");
        if (!path) { publish_result("pushfiles", id, 0, "no server path specified"); return; }
        cmd_pushfiles(id, path);
    }
    else if (strcmp(action, "addfile") == 0) {
        char *id = strtok(NULL, " ");
        if (!id) { publish_result("addfile", NULL, 0, "usage: addfile <guest> <serverPath>"); return; }
        char *path = strtok(NULL, " ");
        if (!path) { publish_result("addfile", id, 0, "no server path specified"); return; }
        cmd_addfile(id, path);
    }
    else if (strcmp(action, "addguest") == 0) {
        char *id = strtok(NULL, " ");
        char *ifs = strtok(NULL, " ");
        char *conf = strtok(NULL, " ");
        char *ip = strtok(NULL, " ");
        if (!id || !ifs || !conf) {
            publish_result("addguest", NULL, 0, "usage: addguest <guest> <ifs_path> <conf_path> [ip]");
            return;
        }
        cmd_addguest(id, ifs, conf, ip ? ip : "");
    }
    else if (strcmp(action, "shellopen") == 0) {
        char *id = strtok(NULL, " ");
        if (!id) { publish_result("shellopen", NULL, 0, "usage: shellopen <guest>"); return; }
        cmd_shellopen(id);
    }
    else if (strcmp(action, "shellwrite") == 0) {
        char *id = strtok(NULL, " ");
        char *rest = strtok(NULL, "");
        if (!id || !rest) { publish_result("shellwrite", NULL, 0, "usage: shellwrite <guest> <data>"); return; }
        cmd_shellwrite(id, rest);
    }
    else if (strcmp(action, "shellclose") == 0) {
        char *id = strtok(NULL, " ");
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

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("\n");
    printf("  Hypervisor Management System v0.2 (MQTT)\n");
    printf("  =========================================\n");

    signal(SIGCHLD, SIG_IGN);

    config_load(&cfg);
    refresh();

    if (hms_mqtt_init(&mqtt, handle_command, NULL) != 0) {
        fprintf(stderr, "[hms] MQTT init failed\n");
        return 1;
    }
    hms_mqtt_connect(&mqtt);

    /* The guest list is published on demand only: in response to a "list"
       command (sent by the GUI's Refresh button) and after start/kill. */
    while (1) {
        hms_mqtt_ensure_connected(&mqtt);
        refresh();
        sleep(2);
    }

    hms_mqtt_disconnect(&mqtt);
    return 0;
}
