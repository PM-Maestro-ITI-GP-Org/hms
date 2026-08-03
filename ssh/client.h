#ifndef HMS_SSH_CLIENT_H
#define HMS_SSH_CLIENT_H

#include "../guest/guest.h"

/*
 * Execute a command inside a guest via SSH.
 *
 * Uses popen("ssh user@ip command") under the hood.
 * Returns a heap-allocated string with the command output,
 * or NULL on failure. Caller must free() the result.
 */
char *ssh_exec(const Guest *g, const char *command);

/*
 * Copy a local file into a guest via SCP.
 * Uses the same auth selection as ssh_exec (key / sshpass / agent).
 * Returns 0 on success, non-zero otherwise.
 */
int ssh_scp_to(const Guest *g, const char *local_path, const char *remote_path,
               char *errbuf, size_t errbuf_sz);

/*
 * Check if a guest is reachable over SSH.
 * Returns 1 if reachable, 0 otherwise.
 */
int ssh_ping(const Guest *g);

#endif
