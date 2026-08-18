#include "guest.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

const char *guest_type_str(GuestType t) {
    switch (t) {
        case GUEST_QNX:     return "qnx";
        case GUEST_LINUX:   return "linux";
        case GUEST_ANDROID: return "android";
        default:            return "unknown";
    }
}

const char *guest_state_str(GuestState s) {
    switch (s) {
        case GUEST_RUNNING: return "running";
        case GUEST_STOPPED: return "stopped";
        case GUEST_CRASHED: return "crashed";
        default:            return "unknown";
    }
}

int guest_id_is_valid(const char *id)
{
    if (!id || !id[0]) return 0;
    if (id[0] == '.') return 0;
    if (strstr(id, "..")) return 0;
    if (strlen(id) >= GUEST_ID_LEN) return 0;

    for (const char *p = id; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')
            continue;
        return 0;
    }
    return 1;
}

GuestType guest_type_from_conf(const char *conf_path) {
    FILE *f = fopen(conf_path, "r");
    if (!f) return GUEST_UNKNOWN;

    char line[256];
    GuestType type = GUEST_UNKNOWN;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "kernel ") || strstr(line, "kernel="))
            { type = GUEST_LINUX;   break; }
        if (strstr(line, "bootimg ") || strstr(line, "bootimg="))
            { type = GUEST_ANDROID; break; }
        if (strstr(line, "load ") || strstr(line, "load\t"))
            { type = GUEST_QNX;     break; }
    }
    fclose(f);
    return type;
}

int guest_boot_image(const Guest *g, char *out, size_t sz)
{
    char dir[GUEST_PATH_LEN];
    snprintf(dir, sizeof(dir), "/guests/%s", g->id);

    char found[GUEST_PATH_LEN] = "";

    /* 1) Resolve from the qvmconf directive (authoritative). */
    FILE *f = fopen(g->conf_path[0] ? g->conf_path : "", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;

            if (strncmp(p, "load ", 5) == 0) {          /* QNX: load <name>.ifs */
                char *tok = strtok(p + 5, " \t\r\n");
                if (tok && strstr(tok, ".ifs")) {
                    snprintf(found, sizeof(found), "%s", tok);
                    break;
                }
            } else if (strncmp(p, "kernel ", 7) == 0) {  /* Linux: kernel <img> */
                char *tok = strtok(p + 7, " \t\r\n");
                if (tok && (strstr(tok, ".img") || strncmp(tok, "vmlinuz", 7) == 0)) {
                    snprintf(found, sizeof(found), "%s", tok);
                    break;
                }
            } else if (strncmp(p, "bootimg ", 8) == 0) { /* Android */
                char *tok = strtok(p + 8, " \t\r\n");
                if (tok) { snprintf(found, sizeof(found), "%s", tok); break; }
            }
        }
        fclose(f);
    }
    if (found[0]) { snprintf(out, sz, "%s", found); return 0; }

    /* 2) Fall back to scanning the guest directory. */
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        char best[GUEST_PATH_LEN] = "";
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            if (strstr(e->d_name, ".ifs") ||
                strcmp(e->d_name, "boot.img") == 0 ||
                strcmp(e->d_name, "bootimg") == 0 ||
                strncmp(e->d_name, "vmlinuz", 7) == 0) {
                if (best[0] == '\0')
                    snprintf(best, sizeof(best), "%s", e->d_name);
            }
        }
        closedir(d);
        if (best[0]) { snprintf(out, sz, "%s", best); return 0; }
    }
    return -1;
}

int guest_system_name(const Guest *g, char *out, size_t sz)
{
    if (!g->conf_path[0]) return -1;

    FILE *f = fopen(g->conf_path, "r");
    if (!f) return -1;

    int rc = -1;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#') continue;
        /* "system <name>" at the top level of the qvmconf. Not "system=": qvm
           takes the value as an argument, not an assignment. */
        if (strncmp(p, "system", 6) != 0) continue;
        if (p[6] != ' ' && p[6] != '\t') continue;

        char *tok = strtok(p + 6, " \t\r\n");
        if (tok && *tok) {
            snprintf(out, sz, "%s", tok);
            rc = 0;
        }
        break;
    }
    fclose(f);
    return rc;
}

const char *guest_meta_conf(const Guest *g)
{
    static char buf[GUEST_PATH_LEN];
    snprintf(buf, sizeof(buf), "/guests/%s/.hms_metadata", g->id);
    return buf;
}

/* Read the qvmconf's `ram <base>,<size>` directive (e.g. "ram 0x80000000,4G")
   and write the configured guest memory size in bytes to out ("" on error).
   The guest kernel's own memory reporting understates this figure (a 4 GB
   guest reports ~3.84 GB as MemTotal), so the config is the authoritative
   size for the Monitor's RAM tile. */
void guest_conf_ram(const Guest *g, char *out, size_t sz)
{
    out[0] = '\0';
    if (!g->conf_path[0]) return;

    FILE *f = fopen(g->conf_path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "ram ", 4) != 0) continue;

        char *comma = strchr(p + 4, ',');
        if (comma) {
            char *tok = comma + 1;
            while (*tok == ' ' || *tok == '\t') tok++;
            char *end = tok;
            while (*end && *end != '\r' && *end != '\n' &&
                   *end != ' ' && *end != '\t') end++;

            size_t n = (size_t)(end - tok);
            if (n > 0 && n < 32) {
                char num[32];
                memcpy(num, tok, n);
                num[n] = '\0';

                char suffix = num[n - 1];
                long long v = strtoll(num, NULL, 10);
                if (suffix == 'K' || suffix == 'k')      v *= 1024LL;
                else if (suffix == 'M' || suffix == 'm') v *= 1024LL * 1024;
                else if (suffix == 'G' || suffix == 'g') v *= 1024LL * 1024 * 1024;
                else if (suffix == 'T' || suffix == 't') v *= 1024LL * 1024 * 1024 * 1024;
                snprintf(out, sz, "%lld", v);
            }
        }
        break;
    }
    fclose(f);
}
