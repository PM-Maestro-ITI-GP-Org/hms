#ifndef HMS_LIFECYCLE_H
#define HMS_LIFECYCLE_H

#include "guest.h"

/*
 * Start a guest by launching QVM with its config file.
 * Returns 0 on success, -1 on error.
 */
int guest_start(const Guest *g);

/*
 * Kill (force-terminate) a guest by its QVM PID.
 * Returns 0 on success, -1 on error.
 */
int guest_kill(const Guest *g);

/*
 * Restart a guest (kill + start).
 * Returns 0 on success, -1 on error.
 */
int guest_restart(const Guest *g);

/*
 * Persist the guest's IP address to its .hms_metadata file
 * (ip= key; the file also holds ssh_* settings and the qvm pid=).
 */
void guest_set_ip(const Guest *g);

/*
 * Set one key=value line in /guests/<id>/.hms_metadata, preserving the rest.
 * Written atomically (temp file + rename).
 */
void guest_meta_set(const Guest *g, const char *key, const char *val);

#endif
