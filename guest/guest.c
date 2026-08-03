#include "guest.h"
#include <string.h>
#include <stdio.h>
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

const char *guest_meta_conf(const Guest *g)
{
    static char buf[GUEST_PATH_LEN];
    snprintf(buf, sizeof(buf), "/guests/%s/.hms_metadata", g->id);
    return buf;
}
