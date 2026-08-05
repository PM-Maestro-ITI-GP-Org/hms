#ifndef HMS_GUEST_H
#define HMS_GUEST_H

#include <time.h>

#define GUEST_ID_LEN     32
#define GUEST_NAME_LEN   64
#define GUEST_TYPE_LEN   16
#define GUEST_STATE_LEN  16
#define GUEST_PATH_LEN   256
#define GUEST_IP_LEN     64
#define GUEST_USER_LEN   64
#define GUEST_PASS_LEN   128
#define GUEST_KEY_LEN    512
#define MAX_GUESTS       16

typedef enum {
    GUEST_QNX,
    GUEST_LINUX,
    GUEST_ANDROID,
    GUEST_UNKNOWN
} GuestType;

typedef enum {
    GUEST_RUNNING,
    GUEST_STOPPED,
    GUEST_CRASHED
} GuestState;

typedef struct {
    char        id[GUEST_ID_LEN];
    char        name[GUEST_NAME_LEN];
    GuestType   type;
    GuestState  state;
    int         pid;
    char        conf_path[GUEST_PATH_LEN];
    char        boot_path[GUEST_PATH_LEN];
    char        rootfs_path[GUEST_PATH_LEN];
    char        ip[GUEST_IP_LEN];
    char        ssh_user[GUEST_USER_LEN];
    char        ssh_password[GUEST_PASS_LEN];
    char        ssh_key[GUEST_KEY_LEN];
    int         ssh_port;
    time_t      name_ts;   /* last attempt to fetch the SSH hostname */
    /* The qvmconf's `system` name, e.g. "guest_1". qvm publishes
       /dev/qvm/<system> while the guest runs, which is how a guest started
       outside HMS is recognised -- see guest_system_name(). */
    char        system_name[GUEST_NAME_LEN];
    /* Running, but HMS did not start it: someone launched qvm from the console
       or a boot script. Nothing is refused because of this; it exists so the
       adoption can be logged once rather than every refresh cycle. */
    int         adopted;
} Guest;

const char *guest_type_str(GuestType t);
const char *guest_state_str(GuestState s);
GuestType   guest_type_from_conf(const char *conf_path);

/*
 * Discover the guest's boot image filename (e.g. "qnx-guest.ifs" for QNX,
 * "boot.img"/"kernel" for Linux). The name is resolved from the qvmconf's
 * `load`/`kernel`/`bootimg` directive when present, otherwise by scanning the
 * guest directory. Returns 0 and writes the name if found, -1 otherwise.
 */
int guest_boot_image(const Guest *g, char *out, size_t sz);

/* Return the guest's HMS metadata path (.hms_metadata: ip=, ssh_*, pid=). */
const char *guest_meta_conf(const Guest *g);

/*
 * Read the `system` name out of the guest's qvmconf -- the name qvm publishes
 * itself under as /dev/qvm/<name>. Returns 0 and writes the name, -1 if the
 * file has no `system` directive.
 */
int guest_system_name(const Guest *g, char *out, size_t sz);

#endif
