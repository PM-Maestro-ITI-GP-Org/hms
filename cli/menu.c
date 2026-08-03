#include "menu.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int menu_show(void)
{
    printf("\n");
    printf("  ==========================================\n");
    printf("     Hypervisor Management System (HMS)\n");
    printf("  ==========================================\n");
    printf("    1. List guests\n");
    printf("    2. Start guest\n");
    printf("    3. Kill guest\n");
    printf("    4. Execute command on guest (SSH)\n");
    printf("    5. Exit\n");
    printf("  ==========================================\n");
    printf("  Choice: ");

    int ch = getchar();
    while (getchar() != '\n'); /* consume rest of line */
    return ch;
}

void menu_list_guests(const Guest guests[MAX_GUESTS], int count)
{
    printf("\n");
    printf("  %-12s %-8s %-8s %-6s %-18s %s\n",
           "Guest", "Type", "State", "PID", "IP", "Conf");
    printf("  %-12s %-8s %-8s %-6s %-18s %s\n",
           "-----", "----", "-----", "---", "--", "----");

    for (int i = 0; i < count; i++) {
        printf("  %-12s %-8s %-8s %-6d %-18s %s\n",
               guests[i].id,
               guest_type_str(guests[i].type),
               guest_state_str(guests[i].state),
               guests[i].pid,
               guests[i].ip[0] ? guests[i].ip : "-",
               guests[i].conf_path);
    }

    if (count == 0) {
        printf("  (no guests found under /guests/)\n");
    }
    printf("\n");
}

void menu_prompt_guest(char *buf, size_t size)
{
    printf("  Guest ID (e.g. guest-1): ");
    if (!fgets(buf, (int)size, stdin)) {
        buf[0] = '\0';
        return;
    }
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
}

void menu_prompt_command(char *buf, size_t size)
{
    printf("  Command: ");
    if (!fgets(buf, (int)size, stdin)) {
        buf[0] = '\0';
        return;
    }
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
}

void menu_pause(const char *msg)
{
    if (msg) printf("%s", msg);
    printf("  Press Enter to continue...");
    while (getchar() != '\n');
}
