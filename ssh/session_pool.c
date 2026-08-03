#include "session_pool.h"
#include <string.h>
#include <stdio.h>

#define POOL_SIZE 16

static SshSession pool[POOL_SIZE];
static int pool_count = 0;

int session_pool_get(const Guest *g, SshSession *s)
{
    /* Look for existing session */
    for (int i = 0; i < pool_count; i++) {
        if (strcmp(pool[i].guest_id, g->id) == 0) {
            *s = pool[i];
            return 0;
        }
    }

    /* Create new entry (popen-based: just store the id) */
    if (pool_count < POOL_SIZE) {
        strncpy(pool[pool_count].guest_id, g->id, GUEST_ID_LEN);
        pool[pool_count].fd = -1;
        *s = pool[pool_count];
        pool_count++;
        return 0;
    }

    printf("  [ssh] session pool full\n");
    return -1;
}

void session_pool_close(const char *guest_id)
{
    for (int i = 0; i < pool_count; i++) {
        if (strcmp(pool[i].guest_id, guest_id) == 0) {
            pool[i] = pool[--pool_count];
            return;
        }
    }
}

void session_pool_close_all(void)
{
    pool_count = 0;
}
