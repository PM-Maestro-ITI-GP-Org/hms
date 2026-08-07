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
 * As ssh_exec, but reports why it failed.
 *
 * On NULL, errbuf holds ssh's own stderr (\"Permission denied (publickey)\",
 * \"No route to host\", ...) rather than the caller having to guess. Without
 * this every failure reached the GUI as the same \"(no output / SSH failed)\",
 * which is a message that cannot be acted on.
 *
 * stderr is captured separately from stdout on purpose: callers parse stdout,
 * so merging the two would corrupt it.
 */
char *ssh_exec_diag(const Guest *g, const char *command,
                    char *errbuf, size_t errbuf_sz);

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

/*
 * Build the shared ssh/scp option string for a guest (host-key policy,
 * timeouts, port, and -i when the guest has a usable key).
 *
 * Exported because shell.c was assembling its own, and had drifted: it still
 * passed StrictHostKeyChecking=no with UserKnownHostsFile=/dev/null, the exact
 * pair this file was changed away from, so the interactive shell accepted
 * whatever answered on the address and kept no record of it.
 *
 * scp needs -P for the port; ssh needs -p. Pass for_scp accordingly.
 */
void ssh_build_opts(const Guest *g, char *opts, size_t sz, int for_scp);

#endif
