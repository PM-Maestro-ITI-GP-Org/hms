#ifndef HMS_SSH_SHELL_H
#define HMS_SSH_SHELL_H

#include "../guest/guest.h"
#include "../mqtt/mqtt_client.h"

#define SHELL_MAX_SESSIONS 4

/*
 * Persistent interactive shell on a running guest.
 *
 * Each session runs a detached "ssh -T user@guest" child; stdin is written
 * with shell_write(), stdout/stderr stream back over MQTT in chunks:
 *   {"state":"shell_out","guest":"<id>","data":"<escaped>"}
 * The session ends (and the slot is freed) when the ssh process exits, and
 *   {"state":"shell_closed","guest":"<id>","msg":"..."}
 * is published.
 */

/* Open a session to a running guest. Returns 0 on success; on failure the
 * reason is written to errbuf. */
int shell_open(hms_mqtt_t *mqtt, const Guest *g,
               char *errbuf, size_t errbuf_sz);

/* Write raw data to the session's stdin. Returns 0 on success. */
int shell_write(const char *guest_id, const char *data,
                char *errbuf, size_t errbuf_sz);

/* Terminate the session for a guest. Returns 0 if a session was closed. */
int shell_close(const char *guest_id);

int shell_is_open(const char *guest_id);
void shell_close_all(void);

#endif
