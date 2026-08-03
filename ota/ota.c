/*
 * ota.c
 * Over-The-Air update for guests.
 *
 * The GUI first uploads the update package to the server
 * (maxmaster@139.185.38.211:/home/maxmaster/uploads/) with SCP, then
 * sends "ota <guest> <remote_path>" over MQTT. This module pulls the
 * package from the server, applies it into /guests/<guest>/ and
 * restarts the guest. All work runs on a detached thread so the MQTT
 * command loop is never blocked.
 */
#include "ota.h"
#include "../guest/guest.h"
#include "../ssh/client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <errno.h>
#include <pthread.h>
#include <limits.h>

#define OTA_STAGE_DIR "/tmp/ota"
#define REMOTE_UPLOAD_DIR "/home/maxmaster/uploads"
#define SCP_COMMON_OPTS "-o StrictHostKeyChecking=no -o ConnectTimeout=10 -o ServerAliveInterval=10 -o ServerAliveCountMax=60"

typedef struct {
    hms_mqtt_t *mqtt;
    char guest_id[GUEST_ID_LEN];
    char remote_path[1024];
    char server[256];      /* user@host of the jump server */
    char ssh_key[512];     /* SSH key on the host for the server */
    int (*kill_guest)(const char *id);
    int (*start_guest)(const char *id);
} ota_job_t;

static void ota_report(hms_mqtt_t *mqtt, const char *guest_id,
                       const char *stage, int progress, const char *msg)
{
    char buf[1536];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"ota_progress\",\"guest\":\"%s\","
             "\"stage\":\"%s\",\"progress\":%d,\"msg\":\"%s\"}",
             guest_id, stage, progress, msg);
    hms_mqtt_publish_status(mqtt, buf);
}

static void msleep(long ms);

/* Run a shell command and return its exit code. SIGCHLD is SIG_IGN so
 * children are auto-reaped and waitpid()/system() are unusable — capture
 * the exit code in a temp file instead. */
static int run_cmd(const char *fmt, ...)
{
    char cmd[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);

    printf("[ota] exec: %s\n", cmd);

    static unsigned int seq = 0;
    char rcfile[128];
    snprintf(rcfile, sizeof(rcfile), "/tmp/ota_rc_%d_%u", getpid(), seq++);

    char wrapped[9000];
    snprintf(wrapped, sizeof(wrapped), "%s; echo $? > %s", cmd, rcfile);

    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", wrapped, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        fprintf(stderr, "[ota] fork failed: %s\n", strerror(errno));
        return -1;
    }

    /* Wait for the child to finish (auto-reaped; kill(pid,0) goes ESRCH). */
    while (kill(pid, 0) == 0)
        msleep(100);

    int rc = -1;
    FILE *f = fopen(rcfile, "r");
    if (f) {
        if (fscanf(f, "%d", &rc) != 1) rc = -1;
        fclose(f);
        remove(rcfile);
    }
    return rc;
}

static void msleep(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void mkdirs(const char *path)
{
    char tmp[3200];
    snprintf(tmp, sizeof(tmp), "mkdir -p %s", path);
    (void)run_cmd("%s", tmp);
}

static long long remote_size(const char *server, const char *ssh_key,
                             const char *remote_path)
{
    char resolved_key[1024];
    /* ssh_key may be stored as '.ssh/id_ed25519', '/.ssh/id_ed25519' or
     * '/root/.ssh/id_ed25519' — normalize to the root home directory. */
    if (strncmp(ssh_key, "/root/", 6) == 0)
        snprintf(resolved_key, sizeof(resolved_key), "%s", ssh_key);
    else if (ssh_key[0] == '/')
        snprintf(resolved_key, sizeof(resolved_key), "/root%s", ssh_key);
    else
        snprintf(resolved_key, sizeof(resolved_key), "/root/%s", ssh_key);
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "ssh " SCP_COMMON_OPTS " -i %s %s stat -c %%s \"%s\" 2>/dev/null",
             resolved_key, server, remote_path);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    char buf[64];
    if (!fgets(buf, sizeof(buf), fp)) { pclose(fp); return -1; }
    pclose(fp);
    long long sz = atoll(buf);
    return sz > 0 ? sz : -1;
}

static int is_archive(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;
    if (strcmp(ext, ".tar") == 0 || strcmp(ext, ".gz") == 0 ||
        strcmp(ext, ".tgz") == 0 || strcmp(ext, ".bz2") == 0 ||
        strcmp(ext, ".xz") == 0) return 1;
    if (strstr(path, ".tar.gz") || strstr(path, ".tar.bz2"))
        return 1;
    return 0;
}

/*
 * Pull the package from the server to the target with SCP.
 * Reports download progress by polling the partial local file size.
 */
static int ota_download(const ota_job_t *j, const char *local_file,
                        long long remote_sz)
{
    char resolved_key[1024];
    /* Normalize key path to root home (see remote_size above). */
    if (strncmp(j->ssh_key, "/root/", 6) == 0)
        snprintf(resolved_key, sizeof(resolved_key), "%s", j->ssh_key);
    else if (j->ssh_key[0] == '/')
        snprintf(resolved_key, sizeof(resolved_key), "/root%s", j->ssh_key);
    else
        snprintf(resolved_key, sizeof(resolved_key), "/root/%s", j->ssh_key);
    char scp_cmd[4096];
    snprintf(scp_cmd, sizeof(scp_cmd),
             "scp -C " SCP_COMMON_OPTS " -i %s \"%s:%s\" \"%s\"",
             resolved_key, j->server, j->remote_path, local_file);

    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", scp_cmd, (char *)NULL);
        _exit(127);
    }

    int last_pct = -1;
    time_t last_report = 0;

    /* SIGCHLD is SIG_IGN (children auto-reaped), so waitpid() always fails
     * with ECHILD. Poll liveness with kill(pid,0) instead; success is
     * determined by the local file reaching the remote size. */
    while (kill(pid, 0) == 0) {
        time_t now = time(NULL);
        if (now - last_report >= 5) {
            last_report = now;
            struct stat st;
            long long local_sz = 0;
            if (stat(local_file, &st) == 0) local_sz = st.st_size;
            if (remote_sz > 0 && local_sz > 0) {
                int pct = (int)(local_sz * 100 / remote_sz);
                if (pct > 100) pct = 100;
                 if (pct != last_pct) {
                     last_pct = pct;
                     char msg[256];
                     snprintf(msg, sizeof(msg), "Pulled %lld of %lld bytes",
                              local_sz, remote_sz);
                     ota_report(j->mqtt, j->guest_id, "download", pct, msg);
                 }
            }
        }
        msleep(200);
    }
    struct stat st;
    long long local_sz = 0;
    if (stat(local_file, &st) == 0) local_sz = st.st_size;
    if (remote_sz > 0 ? local_sz >= remote_sz : local_sz > 0)
        return 0;
    fprintf(stderr, "[ota] SCP pull failed (got %lld, expected %lld)\n",
            local_sz, remote_sz);
    return -1;
}

/*
 * Apply the package into /guests/<guest>/:
 *  - archives are extracted (a single top-level dir inside the archive
 *    is treated as the payload root),
 *  - plain files are copied as-is into the guest directory.
 */
static int ota_apply(const ota_job_t *j, const char *pkg, const char *guest_dir,
                     char *payload_out, size_t payload_sz)
{
    char stage[GUEST_PATH_LEN + 1100];
    snprintf(stage, sizeof(stage), "%s/%s/stage", OTA_STAGE_DIR, j->guest_id);
    mkdirs(stage);

    if (is_archive(pkg)) {
        ota_report(j->mqtt, j->guest_id, "extract", 0, "Extracting package");
        const char *taropt = "xzf";
        if (strstr(pkg, ".tar") && !strstr(pkg, ".gz"))
            taropt = "xf";
        int rc = run_cmd("tar -%s -C %s -f %s", taropt, stage, pkg);
        if (rc != 0) {
            ota_report(j->mqtt, j->guest_id, "extract", 0, "Extraction failed");
            return -1;
        }

        /* If the archive contains a single top-level directory, use it as payload root */
        snprintf(payload_out, payload_sz, "%s", stage);
        DIR *d = opendir(stage);
        if (d) {
            char only[GUEST_PATH_LEN + 1400] = "";
            int count = 0;
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.') continue;
                count++;
                snprintf(only, sizeof(only), "%s/%s", stage, e->d_name);
            }
            closedir(d);
            if (count == 1) {
                struct stat st;
                if (stat(only, &st) == 0 && S_ISDIR(st.st_mode))
                    snprintf(payload_out, payload_sz, "%s", only);
            }
        }
    } else {
        ota_report(j->mqtt, j->guest_id, "extract", 0, "Copying file into guest directory");
        char dst[GUEST_PATH_LEN + 1400];
        const char *name = strrchr(pkg, '/');
        name = name ? name + 1 : pkg;
        snprintf(dst, sizeof(dst), "%s/%s", guest_dir, name);
        snprintf(payload_out, payload_sz, "%s", dst);
        return run_cmd("cp -f \"%s\" \"%s\"", pkg, dst);
    }

    return 0;
}

static void *ota_thread(void *arg)
{
    ota_job_t *j = (ota_job_t *)arg;
    char guest_dir[GUEST_PATH_LEN];
    char pkg[GUEST_PATH_LEN + 1100];
    char payload[GUEST_PATH_LEN + 1400] = "";
    long long remote_sz = -1;

    snprintf(guest_dir, sizeof(guest_dir), "/guests/%s", j->guest_id);

    const char *fname = strrchr(j->remote_path, '/');
    fname = fname ? fname + 1 : j->remote_path;
    snprintf(pkg, sizeof(pkg), "%s/%s/package/%s",
             OTA_STAGE_DIR, j->guest_id, fname);

    ota_report(j->mqtt, j->guest_id, "download", 0, "Starting pull from server");

    remote_sz = remote_size(j->server, j->ssh_key, j->remote_path);
    if (ota_download(j, pkg, remote_sz) != 0) {
        ota_report(j->mqtt, j->guest_id, "failed", 0, "Failed to pull package from server");
        goto done;
    }
    ota_report(j->mqtt, j->guest_id, "download", 100, "Package pulled");

    ota_report(j->mqtt, j->guest_id, "prep", 0, "Stopping guest before applying update");
    if (j->kill_guest && j->kill_guest(j->guest_id) != 0) {
        /* continue anyway: guest may already be stopped */
    }
    msleep(300);

    if (ota_apply(j, pkg, guest_dir, payload, sizeof(payload)) != 0) {
        ota_report(j->mqtt, j->guest_id, "failed", 0, "Failed to apply update");
        goto done;
    }
    ota_report(j->mqtt, j->guest_id, "apply", 100, "Update applied");

    ota_report(j->mqtt, j->guest_id, "restart", 0, "Restarting guest");
    int ok = (j->start_guest && j->start_guest(j->guest_id) == 0);

    {
        char buf[1536];
        snprintf(buf, sizeof(buf),
                 "{\"state\":\"ota_result\",\"guest\":\"%s\",\"success\":%s,"
                 "\"msg\":\"%s\"}",
                 j->guest_id, ok ? "true" : "false",
                 ok ? "OTA update applied and guest restarted"
                    : "Update applied but guest restart failed");
        hms_mqtt_publish_status(j->mqtt, buf);
    }

done:
    free(j);
    return NULL;
}

int ota_start(hms_mqtt_t *mqtt,
              const HmsConfig *cfg,
              const char *guest_id,
              const char *remote_path,
              int (*kill_guest)(const char *id),
              int (*start_guest)(const char *id))
{
    ota_job_t *j = calloc(1, sizeof(*j));
    if (!j) return -1;

    j->mqtt = mqtt;
    snprintf(j->guest_id, sizeof(j->guest_id), "%s", guest_id);
    snprintf(j->remote_path, sizeof(j->remote_path), "%s", remote_path);
    snprintf(j->server, sizeof(j->server), "%s",
             cfg->ota_server[0] ? cfg->ota_server : "maxmaster@139.185.38.211");
    snprintf(j->ssh_key, sizeof(j->ssh_key), "%s",
             cfg->ota_server_key[0] ? cfg->ota_server_key : "/.ssh/id_ed25519");
    j->kill_guest = kill_guest;
    j->start_guest = start_guest;

    pthread_t tid;
    if (pthread_create(&tid, NULL, ota_thread, j) != 0) {
        free(j);
        return -1;
    }
    pthread_detach(tid);
    return 0;
}

/* ======================= Flat file replacement (apply) ======================= */

typedef struct {
    hms_mqtt_t *mqtt;
    char        guest_id[GUEST_ID_LEN];
    char        dest_dir[GUEST_PATH_LEN];     /* /guests/<id> */
    char        stage_dir[GUEST_PATH_LEN + 128];
    char        server[256];
    char        ssh_key[512];
    int         n_paths;
    char        paths[OTA_APPLY_MAX_FILES][1024];
    int         restart;
    int       (*kill_guest)(const char *id);
    int       (*start_guest)(const char *id);
} ota_apply_job_t;

static int is_control_file(const char *name)
{
    if (strncmp(name, "qvm.", 4) == 0) return 1;        /* qvm.pid, qvm.log, ... */
    if (strcmp(name, ".hms_metadata") == 0) return 1;   /* HMS metadata (ip/ssh/pid) */
    return 0;
}

static int is_image_file(const char *name)
{
    if (strstr(name, ".ifs"))              return 1;     /* QNX boot IFS   */
    if (strcmp(name, "boot.img") == 0)    return 1;     /* Linux          */
    if (strcmp(name, "bootimg") == 0)     return 1;     /* Android        */
    if (strncmp(name, "vmlinuz", 7) == 0) return 1;
    if (strcmp(name, "rootfs.img") == 0)  return 1;
    return 0;
}

static const char *base_only(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* Pull a single remote file into local_file with coarse progress.
 * Reports under the given stage name ("download" for fetch, "pushfiles"
 * for send-to-guest). */
static int pull_file(const char *server, const char *ssh_key,
                     const char *remote, const char *local_file,
                     long long remote_sz, const char *guest_id,
                     hms_mqtt_t *mqtt, const char *stage,
                     int file_idx, int n_files)
{
    char resolved_key[1024];
    /* Normalize key path to root home (see remote_size above). */
    if (strncmp(ssh_key, "/root/", 6) == 0)
        snprintf(resolved_key, sizeof(resolved_key), "%s", ssh_key);
    else if (ssh_key[0] == '/')
        snprintf(resolved_key, sizeof(resolved_key), "/root%s", ssh_key);
    else
        snprintf(resolved_key, sizeof(resolved_key), "/root/%s", ssh_key);
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "scp -C " SCP_COMMON_OPTS " -i %s \"%s:%s\" \"%s\"",
             resolved_key, server, remote, local_file);

    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    int last_pct = -1;
    time_t last_report = 0;

    /* SIGCHLD is SIG_IGN (children auto-reaped), so waitpid() always fails
     * with ECHILD. Poll liveness with kill(pid,0) instead; success is
     * determined by the local file reaching the remote size. */
    while (kill(pid, 0) == 0) {
        time_t now = time(NULL);
        if (now - last_report >= 2) {
            last_report = now;
            struct stat st;
            long long local_sz = 0;
            if (stat(local_file, &st) == 0) local_sz = st.st_size;
            int file_pct = (remote_sz > 0 && local_sz > 0)
                ? (int)(local_sz * 100 / remote_sz) : 0;
            if (file_pct > 100) file_pct = 100;
            int overall = (n_files > 1)
                ? (file_idx * 100 + file_pct) / n_files
                : file_pct;
            if (overall != last_pct) {
                last_pct = overall;
                char msg[256];
                snprintf(msg, sizeof(msg), "Pulling %.100s (%lld/%lld KB)",
                         base_only(remote), local_sz / 1024, remote_sz / 1024);
                ota_report(mqtt, guest_id, stage, overall, msg);
            }
        }
        msleep(200);
    }

    /* Child is gone (auto-reaped). Verify the transfer landed. */
    struct stat st;
    long long local_sz = 0;
    if (stat(local_file, &st) == 0) local_sz = st.st_size;
    if (remote_sz > 0 ? local_sz >= remote_sz : local_sz > 0)
        return 0;
    fprintf(stderr, "[ota] apply SCP pull failed (got %lld, expected %lld)\n",
            local_sz, remote_sz);
    return -1;
}

/* Phase 1: pull each remote file from the server into the stage dir only.
 * The guest is left untouched; apply must be run separately. */
static void *ota_fetch_thread(void *arg)
{
    ota_apply_job_t *j = (ota_apply_job_t *)arg;
    int i;

    mkdirs(j->stage_dir);

    for (i = 0; i < j->n_paths; i++) {
        const char *remote = j->paths[i];
        const char *name = base_only(remote);

        if (is_control_file(name)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Refused to fetch control file '%.120s'", name);
            ota_report(j->mqtt, j->guest_id, "failed", 0, msg);
            goto done;
        }

        char local_stage[GUEST_PATH_LEN + 128 + 1024 + 16];
        snprintf(local_stage, sizeof(local_stage), "%s/%s", j->stage_dir, name);

        long long remote_sz = remote_size(j->server, j->ssh_key, remote);
        ota_report(j->mqtt, j->guest_id, "download", 0, "Starting pull from server");
        /* Remove any leftover file from an earlier run so the progress %
         * and final size check reflect only this transfer. */
        remove(local_stage);
        int attempt = 0, ok = 0;
        for (attempt = 1; attempt <= 3 && !ok; attempt++) {
            if (attempt > 1) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Retry %d/3 for %.100s", attempt, name);
                ota_report(j->mqtt, j->guest_id, "download", 0, msg);
                remove(local_stage);
                msleep(1000);
            }
            if (pull_file(j->server, j->ssh_key, remote, local_stage, remote_sz,
                          j->guest_id, j->mqtt, "download", i, j->n_paths) == 0)
                ok = 1;
        }
        if (!ok) {
        ota_report(j->mqtt, j->guest_id, "failed", 0, "Failed to pull file from server after 3 attempts");
            goto done;
        }
        ota_report(j->mqtt, j->guest_id, "download", 100, "Fetched file from server");
    }

    {
        char buf[1536];
        snprintf(buf, sizeof(buf),
                 "{\"state\":\"ota_result\",\"guest\":\"%s\",\"success\":true,"
                 "\"msg\":\"%d file(s) fetched, ready to apply\"}",
                 j->guest_id, j->n_paths);
        hms_mqtt_publish_status(j->mqtt, buf);
    }

done:
    free(j);
    return NULL;
}

/* Phase 2: kill guest, copy staged files into the guest dir, restart the
 * guest, then remove the staged files. */
static void *ota_apply_thread(void *arg)
{
    ota_apply_job_t *j = (ota_apply_job_t *)arg;
    int i;

    /* Enumerate staged files */
    DIR *d = opendir(j->stage_dir);
    if (!d) {
        ota_report(j->mqtt, j->guest_id, "failed", 0,
                   "No staged files found — run Update (fetch) first");
        goto done;
    }

    char names[OTA_APPLY_MAX_FILES][256];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < OTA_APPLY_MAX_FILES) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (is_control_file(e->d_name)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Refused to apply control file '%.120s'", e->d_name);
            ota_report(j->mqtt, j->guest_id, "failed", 0, msg);
            closedir(d);
            goto done;
        }
        snprintf(names[n], sizeof(names[n]), "%s", e->d_name);
        n++;
    }
    closedir(d);

    if (n == 0) {
        ota_report(j->mqtt, j->guest_id, "failed", 0,
                   "No staged files found — run Update (fetch) first");
        goto done;
    }

    /* Kill the guest before touching its files */
    ota_report(j->mqtt, j->guest_id, "restart", 0, "Stopping guest before applying changes");
    if (j->kill_guest && j->kill_guest(j->guest_id) != 0)
        (void)0;  /* continue anyway: guest may already be stopped */
    msleep(300);

    /* Copy staged files into the guest directory */
    for (i = 0; i < n; i++) {
        char dest[GUEST_PATH_LEN + 4096 + 16];
        char local_stage[GUEST_PATH_LEN + 128 + 4096 + 16];
        snprintf(dest, sizeof(dest), "%s/%s", j->dest_dir, names[i]);
        snprintf(local_stage, sizeof(local_stage), "%s/%s", j->stage_dir, names[i]);

        if (run_cmd("cp -f \"%s\" \"%s\"", local_stage, dest) != 0) {
            ota_report(j->mqtt, j->guest_id, "failed", 0,
                       "Failed to place file in guest directory");
            goto done;
        }

        int pct = (int)((i + 1) * 100 / n);
        char msg[256];
        snprintf(msg, sizeof(msg), "Updated %.120s", names[i]);
        ota_report(j->mqtt, j->guest_id, "apply", pct, msg);
    }

    /* Image files always force a restart, regardless of the no-restart flag. */
    int restart = j->restart;
    for (i = 0; i < n; i++)
        if (is_image_file(names[i])) { restart = 1; break; }

    if (restart) {
        ota_report(j->mqtt, j->guest_id, "restart", 50, "Guest stopped, starting now");
        int ok = (j->start_guest && j->start_guest(j->guest_id) == 0);
        ota_report(j->mqtt, j->guest_id, "restart", 100,
                   ok ? "Guest restarted" : "Guest restart failed");
    } else {
        ota_report(j->mqtt, j->guest_id, "restart", 100, "Files replaced (guest left running)");
    }

    /* Clean up the stage dir */
    for (i = 0; i < n; i++) {
        char p[GUEST_PATH_LEN + 128 + 4096 + 16];
        snprintf(p, sizeof(p), "%s/%s", j->stage_dir, names[i]);
        remove(p);
    }

    {
        char buf[1536];
        snprintf(buf, sizeof(buf),
                 "{\"state\":\"ota_result\",\"guest\":\"%s\",\"success\":true,"
                 "\"msg\":\"%d file(s) applied%s\"}",
                 j->guest_id, n,
                 restart ? " and guest restarted" : " (no restart)");
        hms_mqtt_publish_status(j->mqtt, buf);
    }

done:
    free(j);
    return NULL;
}

int ota_fetch_start(hms_mqtt_t *mqtt, const HmsConfig *cfg,
                    const char *guest_id, int n_paths,
                    const char remote_paths[][1024],
                    int (*kill_guest)(const char *id),
                    int (*start_guest)(const char *id))
{
    (void)kill_guest; (void)start_guest;
    if (n_paths <= 0 || n_paths > OTA_APPLY_MAX_FILES) return -1;

    ota_apply_job_t *j = calloc(1, sizeof(*j));
    if (!j) return -1;

    j->mqtt = mqtt;
    snprintf(j->guest_id, sizeof(j->guest_id), "%s", guest_id);
    snprintf(j->dest_dir, sizeof(j->dest_dir), "/guests/%s", guest_id);
    snprintf(j->stage_dir, sizeof(j->stage_dir), "%s/%s/stage", OTA_STAGE_DIR, guest_id);
    snprintf(j->server, sizeof(j->server), "%s",
             cfg->ota_server[0] ? cfg->ota_server : "maxmaster@139.185.38.211");
    snprintf(j->ssh_key, sizeof(j->ssh_key), "%s",
             cfg->ota_server_key[0] ? cfg->ota_server_key : "/.ssh/id_ed25519");
    j->n_paths = n_paths;
    for (int k = 0; k < n_paths; k++)
        snprintf(j->paths[k], sizeof(j->paths[k]), "%s", remote_paths[k]);

    pthread_t tid;
    if (pthread_create(&tid, NULL, ota_fetch_thread, j) != 0) {
        free(j);
        return -1;
    }
    pthread_detach(tid);
    return 0;
}

int ota_apply_start(hms_mqtt_t *mqtt, const HmsConfig *cfg,
                    const char *guest_id, int restart,
                    int (*kill_guest)(const char *id),
                    int (*start_guest)(const char *id))
{
    ota_apply_job_t *j = calloc(1, sizeof(*j));
    if (!j) return -1;

    j->mqtt = mqtt;
    snprintf(j->guest_id, sizeof(j->guest_id), "%s", guest_id);
    snprintf(j->dest_dir, sizeof(j->dest_dir), "/guests/%s", guest_id);
    snprintf(j->stage_dir, sizeof(j->stage_dir), "%s/%s/stage", OTA_STAGE_DIR, guest_id);
    snprintf(j->server, sizeof(j->server), "%s",
             cfg->ota_server[0] ? cfg->ota_server : "maxmaster@139.185.38.211");
    snprintf(j->ssh_key, sizeof(j->ssh_key), "%s",
             cfg->ota_server_key[0] ? cfg->ota_server_key : "/.ssh/id_ed25519");
    j->n_paths = 0;
    j->restart = restart;
    j->kill_guest = kill_guest;
    j->start_guest = start_guest;

    pthread_t tid;
    if (pthread_create(&tid, NULL, ota_apply_thread, j) != 0) {
        free(j);
        return -1;
    }
    pthread_detach(tid);
    return 0;
}

/* ============================ Send files to guest ============================
 * GUI builds a tar.gz locally (each file at its absolute guest path), uploads
 * it to the server, then sends "pushfiles <guest> <serverPath>". We pull the
 * archive from the server, scp it into the running guest and untar it with
 * "tar -xzf ... -C /" inside the guest so every file lands at its path.
 */

typedef struct {
    hms_mqtt_t *mqtt;
    char guest_id[GUEST_ID_LEN];
    char remote_path[1024];
    char server[256];      /* user@host of the jump server */
    char ssh_key[512];     /* SSH key on the host for the server */
    Guest guest;           /* snapshot of the guest's SSH credentials */
} ota_push_job_t;

static void *ota_push_thread(void *arg)
{
    ota_push_job_t *j = (ota_push_job_t *)arg;
    const char *name = base_only(j->remote_path);
    char push_dir[2048], local_tar[3200];
    snprintf(push_dir, sizeof(push_dir), "%s/%s/push", OTA_STAGE_DIR, j->guest_id);
    snprintf(local_tar, sizeof(local_tar), "%s/%s", push_dir, name);

    ota_report(j->mqtt, j->guest_id, "pushfiles", 0,
               "Pulling archive from the server");
    mkdirs(push_dir);
    remove(local_tar);

    long long remote_sz = remote_size(j->server, j->ssh_key, j->remote_path);
    if (remote_sz <= 0) {
        ota_report(j->mqtt, j->guest_id, "failed", 0,
                   "Archive not found on the server");
        free(j);
        return NULL;
    }

    int ok = 0, attempt;
    for (attempt = 1; attempt <= 3 && !ok; attempt++) {
        if (attempt > 1) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Retry %d/3 pulling the archive", attempt);
            ota_report(j->mqtt, j->guest_id, "pushfiles", 0, msg);
            remove(local_tar);
            msleep(1000);
        }
        if (pull_file(j->server, j->ssh_key, j->remote_path, local_tar,
                      remote_sz, j->guest_id, j->mqtt, "pushfiles", 0, 1) == 0)
            ok = 1;
    }
    if (!ok) {
        ota_report(j->mqtt, j->guest_id, "failed", 0,
                   "Failed to pull the archive after 3 attempts");
        free(j);
        return NULL;
    }

    ota_report(j->mqtt, j->guest_id, "pushfiles", 70,
               "Archive pulled — copying into the guest");
    char push_err[512] = "";
    if (ssh_scp_to(&j->guest, local_tar, "/tmp/pushfiles.tar.gz",
                   push_err, sizeof(push_err)) != 0) {
        char msg[600];
        snprintf(msg, sizeof(msg), "Failed to copy the archive into the guest: %s",
                 push_err[0] ? push_err : "unknown error");
        ota_report(j->mqtt, j->guest_id, "failed", 0, msg);
        remove(local_tar);
        free(j);
        return NULL;
    }

    ota_report(j->mqtt, j->guest_id, "pushfiles", 85,
               "Extracting files into the guest filesystem");
    /* ssh_exec() can report failure without any output (QNX pclose quirk),
       so require an explicit EXTRACT_OK marker; tar's stderr is merged into
       the capture so a real failure can be reported to the GUI. */
    char *out = ssh_exec(&j->guest,
        "exec 2>&1; tar -xzf /tmp/pushfiles.tar.gz -C / && rm -f /tmp/pushfiles.tar.gz && echo EXTRACT_OK");
    int success = (out != NULL && strstr(out, "EXTRACT_OK") != NULL);
    char extract_err[768] = "";
    if (!success && out && out[0]) {
        size_t elen = strlen(out);
        if (elen >= sizeof(extract_err))
            memcpy(extract_err, out + elen - sizeof(extract_err) + 1, sizeof(extract_err) - 1);
        else
            memcpy(extract_err, out, elen);
        extract_err[sizeof(extract_err) - 1] = '\0';
        for (char *p = extract_err; *p; ++p)
            if (*p == '\n' || *p == '\r') *p = ' ';
    }
    free(out);
    remove(local_tar);

    char buf[1600];
    if (success) {
        snprintf(buf, sizeof(buf),
                 "{\"state\":\"ota_result\",\"guest\":\"%s\",\"success\":true,"
                 "\"msg\":\"files pushed to guest\"}",
                 j->guest_id);
        ota_report(j->mqtt, j->guest_id, "pushfiles", 100, "Files pushed to guest");
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"state\":\"ota_result\",\"guest\":\"%s\",\"success\":false,"
                 "\"msg\":\"extraction failed inside the guest: %s\"}",
                 j->guest_id, extract_err[0] ? extract_err : "unknown error");
    }
    hms_mqtt_publish_status(j->mqtt, buf);
    free(j);
    return NULL;
}

int ota_push_start(hms_mqtt_t *mqtt, const HmsConfig *cfg,
                   const Guest *g, const char *remote_path)
{
    ota_push_job_t *j = calloc(1, sizeof(*j));
    if (!j) return -1;

    j->mqtt = mqtt;
    snprintf(j->guest_id, sizeof(j->guest_id), "%s", g->id);
    snprintf(j->remote_path, sizeof(j->remote_path), "%s", remote_path);
    snprintf(j->server, sizeof(j->server), "%s",
             cfg->ota_server[0] ? cfg->ota_server : "maxmaster@139.185.38.211");
    snprintf(j->ssh_key, sizeof(j->ssh_key), "%s",
             cfg->ota_server_key[0] ? cfg->ota_server_key : "/.ssh/id_ed25519");
    j->guest = *g;

    pthread_t tid;
    if (pthread_create(&tid, NULL, ota_push_thread, j) != 0) {
        free(j);
        return -1;
    }
    pthread_detach(tid);
    return 0;
}

/* ======================= Add a new partition file (addfile) ======================= */

typedef struct {
    hms_mqtt_t *mqtt;
    char guest_id[GUEST_ID_LEN];
    char remote_path[1024];
    char server[256];
    char ssh_key[512];
} addfile_job_t;

static void publish_addfile_result(addfile_job_t *j, int success, const char *msg)
{
    char buf[1536];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"addfile_result\",\"guest\":\"%s\",\"success\":%s,"
             "\"msg\":\"%s\"}",
             j->guest_id, success ? "true" : "false", msg);
    hms_mqtt_publish_status(j->mqtt, buf);
}

static void *addfile_thread(void *arg)
{
    addfile_job_t *j = (addfile_job_t *)arg;
    const char *name = base_only(j->remote_path);

    if (is_control_file(name)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Refused to add control file '%.120s'", name);
        publish_addfile_result(j, 0, msg);
        goto done;
    }

    char stage_dir[GUEST_PATH_LEN + 128 + 64];
    snprintf(stage_dir, sizeof(stage_dir), "/tmp/ota/%s/addfile", j->guest_id);
    mkdirs(stage_dir);

    char local[2048];
    snprintf(local, sizeof(local), "%s/%s", stage_dir, name);

    char guest_dir[GUEST_PATH_LEN];
    snprintf(guest_dir, sizeof(guest_dir), "/guests/%s", j->guest_id);

    long long remote_sz = remote_size(j->server, j->ssh_key, j->remote_path);
    remove(local);
    int ok = (pull_file(j->server, j->ssh_key, j->remote_path, local, remote_sz,
                        j->guest_id, j->mqtt, "addfile", 0, 1) == 0);
    if (ok) {
        ok = (run_cmd("cp -f \"%s\" \"%s/%s\"", local, guest_dir, name) == 0);
        remove(local);
    }

    if (ok) {
        char msg[480];
        snprintf(msg, sizeof(msg),
                 "Added %.120s to %s (no guest restart needed)", name, guest_dir);
        publish_addfile_result(j, 1, msg);
    } else {
        publish_addfile_result(j, 0, "failed to add the file to the guest directory");
    }

done:
    free(j);
    return NULL;
}

int ota_addfile_start(hms_mqtt_t *mqtt, const HmsConfig *cfg,
                      const char *guest_id, const char *remote_path)
{
    addfile_job_t *j = calloc(1, sizeof(*j));
    if (!j) return -1;

    j->mqtt = mqtt;
    snprintf(j->guest_id, sizeof(j->guest_id), "%s", guest_id);
    snprintf(j->remote_path, sizeof(j->remote_path), "%s", remote_path);
    snprintf(j->server, sizeof(j->server), "%s",
             cfg->ota_server[0] ? cfg->ota_server : "maxmaster@139.185.38.211");
    snprintf(j->ssh_key, sizeof(j->ssh_key), "%s",
             cfg->ota_server_key[0] ? cfg->ota_server_key : "/.ssh/id_ed25519");

    pthread_t tid;
    if (pthread_create(&tid, NULL, addfile_thread, j) != 0) {
        free(j);
        return -1;
    }
    pthread_detach(tid);
    return 0;
}

/* ======================= Create a new guest (addguest) ======================= */

typedef struct {
    hms_mqtt_t *mqtt;
    char guest_id[GUEST_ID_LEN];
    char ifs_remote[1024];
    char conf_remote[1024];
    char ip[GUEST_IP_LEN];
    char server[256];
    char ssh_key[512];
} addguest_job_t;

static void publish_addguest_result(addguest_job_t *j, int success, const char *msg)
{
    char buf[1536];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"addguest_result\",\"guest\":\"%s\",\"success\":%s,"
             "\"msg\":\"%s\"}",
             j->guest_id, success ? "true" : "false", msg);
    hms_mqtt_publish_status(j->mqtt, buf);
}

static void *addguest_thread(void *arg)
{
    addguest_job_t *j = (addguest_job_t *)arg;
    char guest_dir[GUEST_PATH_LEN];
    snprintf(guest_dir, sizeof(guest_dir), "/guests/%s", j->guest_id);

    struct stat st;
    if (stat(guest_dir, &st) == 0) {
        publish_addguest_result(j, 0, "a guest with this ID already exists");
        goto done;
    }

    const char *conf_name = base_only(j->conf_remote);
    const char *ifs_name = base_only(j->ifs_remote);
    if (is_control_file(conf_name) || is_control_file(ifs_name)) {
        publish_addguest_result(j, 0, "refusing control-file names");
        goto done;
    }

    mkdirs(guest_dir);

    char local_conf[GUEST_PATH_LEN + 1024 + 16];
    snprintf(local_conf, sizeof(local_conf), "%s/%s", guest_dir, conf_name);
    char local_ifs[GUEST_PATH_LEN + 1024 + 16];
    snprintf(local_ifs, sizeof(local_ifs), "%s/%s", guest_dir, ifs_name);

    long long sz = remote_size(j->server, j->ssh_key, j->conf_remote);
    int ok = (pull_file(j->server, j->ssh_key, j->conf_remote, local_conf, sz,
                        j->guest_id, j->mqtt, "addguest", 0, 2) == 0);
    if (ok) {
        sz = remote_size(j->server, j->ssh_key, j->ifs_remote);
        ok = (pull_file(j->server, j->ssh_key, j->ifs_remote, local_ifs, sz,
                        j->guest_id, j->mqtt, "addguest", 1, 2) == 0);
    }

    if (ok && j->ip[0]) {
        char meta[GUEST_PATH_LEN + 32];
        snprintf(meta, sizeof(meta), "/guests/%s/.hms_metadata", j->guest_id);
        FILE *f = fopen(meta, "a");
        if (f) {
            fprintf(f, "ip=%s\n", j->ip);
            fclose(f);
        }
    }

    if (ok) {
        char msg[480];
        snprintf(msg, sizeof(msg),
                 "guest '%.32s' created (%.60s, %.60s) — refresh the list",
                 j->guest_id, conf_name, ifs_name);
        publish_addguest_result(j, 1, msg);
    } else {
        publish_addguest_result(j, 0, "failed to pull the guest files from the server");
    }

done:
    free(j);
    return NULL;
}

int ota_addguest_start(hms_mqtt_t *mqtt, const HmsConfig *cfg,
                       const char *guest_id, const char *ifs_remote,
                       const char *conf_remote, const char *ip)
{
    addguest_job_t *j = calloc(1, sizeof(*j));
    if (!j) return -1;

    j->mqtt = mqtt;
    snprintf(j->guest_id, sizeof(j->guest_id), "%s", guest_id);
    snprintf(j->ifs_remote, sizeof(j->ifs_remote), "%s", ifs_remote);
    snprintf(j->conf_remote, sizeof(j->conf_remote), "%s", conf_remote);
    snprintf(j->ip, sizeof(j->ip), "%s", ip ? ip : "");
    snprintf(j->server, sizeof(j->server), "%s",
             cfg->ota_server[0] ? cfg->ota_server : "maxmaster@139.185.38.211");
    snprintf(j->ssh_key, sizeof(j->ssh_key), "%s",
             cfg->ota_server_key[0] ? cfg->ota_server_key : "/.ssh/id_ed25519");

    pthread_t tid;
    if (pthread_create(&tid, NULL, addguest_thread, j) != 0) {
        free(j);
        return -1;
    }
    pthread_detach(tid);
    return 0;
}
