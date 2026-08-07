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
    /*
     * Where OTA packages are downloaded and unpacked before being installed.
     *
     * This was /tmp/ota, and on the host image /tmp is a symlink to
     * /dev/shmem -- that is RAM. Pulling a 256 MB rootfs.img there, then
     * unpacking it there as well, spends half a gigabyte of a board whose
     * remaining memory is only whatever the hypervisor did not hand to the
     * guests. The default is now a directory on the same filesystem as
     * /guests, which is the QNX6 data partition on the SD card: it is disk
     * backed, and the install copy no longer crosses a filesystem.
     *
     * The leading dot keeps it out of discovery, which skips names beginning
     * with '.' and requires a "guest-" prefix besides.
     */
    char ota_stage_dir[GUEST_PATH_LEN];
} HmsConfig;

/*
 * Load config from /etc/hms.conf.
 * If the file doesn't exist, populate with sensible defaults.
 */
void config_load(HmsConfig *cfg);

#endif
