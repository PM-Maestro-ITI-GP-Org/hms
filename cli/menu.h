#ifndef HMS_MENU_H
#define HMS_MENU_H

#include <stddef.h>
#include "../guest/guest.h"

/*
 * Display the main menu and return the user's choice.
 */
int menu_show(void);

/*
 * List all discovered guests in a formatted table.
 */
void menu_list_guests(const Guest guests[MAX_GUESTS], int count);

/*
 * Prompt for a guest ID and return it.
 * Reads into buf (up to size bytes).
 */
void menu_prompt_guest(char *buf, size_t size);

/*
 * Prompt for an SSH command string.
 * Reads into buf (up to size bytes).
 */
void menu_prompt_command(char *buf, size_t size);

/*
 * Print a message and wait for Enter.
 */
void menu_pause(const char *msg);

#endif
