#ifndef HMS_ADDRESS_H
#define HMS_ADDRESS_H

#include "guest.h"
#include <stddef.h>

/*
 * Work out the address to reach this guest on.
 *
 * .hms_metadata's ip= is the only place HMS ever read one from, so a guest
 * whose file has no ip= -- one HMS never started, or one on a data partition
 * that was just written -- was discovered, reported running, and reachable by
 * nothing: every ssh path checks g->ip first and gives up on an empty one.
 *
 * Order, first answer wins:
 *
 *   1. g->ip, if discovery already read one out of .hms_metadata
 *   2. the host-side vdevpeer link bound to this guest (see below)
 *   3. a default for the guest's type -- 10.0.0.2 for QNX, 10.0.1.2 for
 *      Linux and Android, which is what the images configure
 *
 * Returns 0 and writes the address, -1 if none of the three produced one.
 * Does not touch the guest or its metadata; persisting is the caller's job.
 */
int guest_resolve_ip(const Guest *g, char *out, size_t sz);

#endif
