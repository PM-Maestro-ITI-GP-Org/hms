#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void cp_str(char *dst, size_t dst_size, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void config_load(HmsConfig *cfg)
{
    /* defaults */
    cp_str(cfg->guests_path,        sizeof(cfg->guests_path),        "/guests");
    cp_str(cfg->ssh_default_user,   sizeof(cfg->ssh_default_user),   "root");
    cfg->ssh_default_password[0] = '\0';
    cfg->ssh_default_port  = 22;
    cfg->refresh_interval_s = 5;
    cfg->ssh_key_path[0]   = '\0';
    cp_str(cfg->ota_server,     sizeof(cfg->ota_server),     "maxmaster@139.185.38.211");
    cp_str(cfg->ota_server_key, sizeof(cfg->ota_server_key), "/.ssh/id_ed25519");
    cp_str(cfg->ota_stage_dir,  sizeof(cfg->ota_stage_dir),  "/guests/.ota-stage");

    FILE *f = fopen("/etc/hms.conf", "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';

        if (strncmp(line, "guests_path=", 12) == 0)
            cp_str(cfg->guests_path, sizeof(cfg->guests_path), line + 12);
        else if (strncmp(line, "ssh_user=", 9) == 0)
            cp_str(cfg->ssh_default_user, sizeof(cfg->ssh_default_user), line + 9);
        else if (strncmp(line, "ssh_password=", 13) == 0)
            cp_str(cfg->ssh_default_password, sizeof(cfg->ssh_default_password), line + 13);
        else if (strncmp(line, "ssh_port=", 9) == 0)
            cfg->ssh_default_port = atoi(line + 9);
        else if (strncmp(line, "ssh_key=", 8) == 0)
            cp_str(cfg->ssh_key_path, sizeof(cfg->ssh_key_path), line + 8);
        else if (strncmp(line, "refresh_interval=", 17) == 0)
            cfg->refresh_interval_s = atoi(line + 17);
        else if (strncmp(line, "ota_server=", 11) == 0)
            cp_str(cfg->ota_server, sizeof(cfg->ota_server), line + 11);
        else if (strncmp(line, "ota_server_key=", 15) == 0)
            cp_str(cfg->ota_server_key, sizeof(cfg->ota_server_key), line + 15);
        else if (strncmp(line, "ota_stage_dir=", 14) == 0)
            cp_str(cfg->ota_stage_dir, sizeof(cfg->ota_stage_dir), line + 14);
    }
    fclose(f);
}
