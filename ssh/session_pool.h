#ifndef HMS_SESSION_POOL_H
#define HMS_SESSION_POOL_H

#include "../guest/guest.h"

/*
 * Simple SSH session cache.
 * Currently a placeholder — sessions are created/destroyed per command
 * in the popen-based implementation. When switching to libssh, this
 * will hold persistent ssh_session handles.
 */
typedef struct {
    char guest_id[GUEST_ID_LEN];
    int  fd;          /* placeholder for libssh session ptr */
} SshSession;

/*
 * Get or create a session for a guest.
 * Returns 0 on success, -1 on error.
 */
int session_pool_get(const Guest *g, SshSession *s);

/*
 * Close and remove a session.
 */
void session_pool_close(const char *guest_id);

/*
 * Close all sessions.
 */
void session_pool_close_all(void);

#endif
