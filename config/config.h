#ifndef HMS_CONFIG_H
#define HMS_CONFIG_H

#include "../guest/guest.h"

#define MAX_SSH_KEY_LEN   512
#define MAX_SERVER_LEN    256

typedef struct {
    char guests_path[GUEST_PATH_LEN];
    char ssh_default_user[GUEST_USER_LEN];
    char ssh_default_password[GUEST_PASS_LEN];
    int  ssh_default_port;
    char ssh_key_path[MAX_SSH_KEY_LEN];
    int  refresh_interval_s;
    char ota_server[MAX_SERVER_LEN];   /* user@host used for OTA SCP pull */
    char ota_server_key[MAX_SSH_KEY_LEN]; /* SSH key on the host for that server */
} HmsConfig;

/*
 * Load config from /etc/hms.conf.
 * If the file doesn't exist, populate with sensible defaults.
 */
void config_load(HmsConfig *cfg);

#endif
