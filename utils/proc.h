#ifndef HMS_PROC_H
#define HMS_PROC_H

#include <stddef.h>
#include <stdint.h>

/*
 * Scan /proc for all numeric PID directories.
 * Fills pids[] up to max_pids, returns actual count.
 */
int scan_proc(int *pids, int max_pids);

/*
 * Read /proc/<pid>/cmdline into buf (up to size bytes).
 * Returns 1 on success, 0 on error.
 */
int read_proc_cmdline(int pid, char *buf, size_t size);

#endif
