#ifndef HMS_DISCOVERER_H
#define HMS_DISCOVERER_H

#include "guest.h"

/*
 * Scan /guests/ for guest-* directories and match them against
 * running QVM processes in /proc. Populates out[] array.
 * Returns number of guests found (0 … MAX_GUESTS).
 */
int discover_guests(Guest out[MAX_GUESTS]);

/*
 * Refresh state for a single guest by checking /proc.
 */
void refresh_guest_state(Guest *g);

/*
 * Fetch the guest's hostname over SSH (throttled: at most one attempt
 * per 10 s per guest, skipped while the guest is stopped).
 */
void refresh_guest_name(Guest *g);

/*
 * Look up a guest by its ID string (e.g. "guest-1").
 * Returns pointer into the array or NULL.
 */
Guest *find_guest(Guest list[MAX_GUESTS], int count, const char *id);

#endif
