#include "proc.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

int scan_proc(int *pids, int max_pids)
{
    DIR *d = opendir("/proc");
    if (!d) return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && count < max_pids) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
            continue;
        pids[count++] = atoi(entry->d_name);
    }
    closedir(d);
    return count;
}

int read_proc_cmdline(int pid, char *buf, size_t size)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    ssize_t n = read(fd, buf, size - 1);
    close(fd);

    if (n <= 0) return 0;

    buf[n] = '\0';

    /* cmdline uses \0 as separator; replace with spaces for readability */
    for (ssize_t i = 0; i < n - 1; i++) {
        if (buf[i] == '\0') buf[i] = ' ';
    }
    return 1;
}
