#ifndef HMS_OTA_H
#define HMS_OTA_H

#include "../mqtt/mqtt_client.h"
#include "../config/config.h"

/*
 * OTA update flow (Laptop -> Server -> QNX target):
 *   GUI uploads the package to the server via SCP, then sends
 *   "ota <guest> <remote_path>" over MQTT. HMS pulls the package
 *   from the server with SCP (using the SSH key from hms.conf),
 *   extracts it into /guests/<guest>/, then restarts the guest.
 */
int ota_start(hms_mqtt_t *mqtt,
              const HmsConfig *cfg,
              const char *guest_id,
              const char *remote_path,
              int (*kill_guest)(const char *id),
              int (*start_guest)(const char *id));

/*
 * Flat (non-archive) file replacement for "partition" updates.
 *
 * Two-phase flow:
 *   ota_fetch_start(): pulls each remote file from the server into the guest's
 *                      stage dir (/tmp/ota/<guest>/stage/) only — the guest is
 *                      NOT touched.
 *   ota_apply_start(): takes no paths; scans the stage dir, kills the guest,
 *                      copies the staged files into /guests/<guest>/, restarts
 *                      the guest, then removes the staged files.
 *
 * Image files (IFS / boot.img / rootfs.img) always force a restart. Control
 * files (qvm.*, .hms_metadata) are never applied.
 */
#define OTA_APPLY_MAX_FILES 16

int ota_fetch_start(hms_mqtt_t *mqtt,
                    const HmsConfig *cfg,
                    const char *guest_id,
                    int n_paths, const char remote_paths[][1024],
                    int (*kill_guest)(const char *id),
                    int (*start_guest)(const char *id));

int ota_apply_start(hms_mqtt_t *mqtt,
                    const HmsConfig *cfg,
                    const char *guest_id,
                    int restart,
                    int (*kill_guest)(const char *id),
                    int (*start_guest)(const char *id));

/*
 * Send files to a running guest.
 *
 * The GUI packs files (each at its absolute guest path) into one tar.gz,
 * uploads it to the server, then sends "pushfiles <guest> <serverPath>".
 * HMS pulls the archive from the server, scp's it into the guest
 * (/tmp/pushfiles.tar.gz) and runs "tar -xzf ... -C /" inside the guest so
 * every file lands at its path. The guest must be running (SSH into it).
 */
int ota_push_start(hms_mqtt_t *mqtt,
                   const HmsConfig *cfg,
                   const Guest *g,
                   const char *remote_path);

/*
 * Add a NEW partition file (e.g. an extra .img) to /guests/<guest>/ without
 * replacing anything and without touching the running guest. The file is
 * pulled from the server and copied into the guest directory; the result is
 * published as {"state":"addfile_result", ...}.
 */
int ota_addfile_start(hms_mqtt_t *mqtt,
                      const HmsConfig *cfg,
                      const char *guest_id,
                      const char *remote_path);

/*
 * Create a NEW guest: make /guests/<guest>/, pull the boot image (.ifs) and
 * the qvmconf from the server into it (original file names are kept), and
 * record the IP in .hms_metadata when one is given. The result is published
 * as {"state":"addguest_result", ...}.
 */
int ota_addguest_start(hms_mqtt_t *mqtt,
                       const HmsConfig *cfg,
                       const char *guest_id,
                       const char *ifs_remote,
                       const char *conf_remote,
                       const char *ip);

#endif
