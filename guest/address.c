#include "address.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/*
 * Which host interface is this guest's link?
 *
 * vpctl with no assignments reports the binding it already has:
 *
 *     $ vpctl vp0
 *     vp0: peer=/dev/qvm/guest_1/guest_to_host bind=/dev/vdevpeers/vp0 ...
 *
 * and the peer path carries the qvm system name, which is the same name
 * discovery reads out of the guest's qvmconf. That makes the mapping a fact
 * read off the running host rather than an assumption about which vp goes
 * where -- vp0 is guest-1 only because .vdev_net_start.sh happens to bind them
 * in that order, and a third guest would make that ordering meaningless.
 */
static int iface_peers_guest(const char *iface, const char *system_name)
{
    if (!system_name || !system_name[0])
        return 0;

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "vpctl %s 2>/dev/null", iface);

    FILE *fp = popen(cmd, "r");
    if (!fp)
        return 0;

    /* The trailing slash matters: /dev/qvm/guest_1/ must not match a system
       called guest_10. */
    char want[GUEST_NAME_LEN + 16];
    snprintf(want, sizeof(want), "/dev/qvm/%s/", system_name);

    char line[512];
    int hit = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, want)) {
            hit = 1;
            break;
        }
    }
    pclose(fp);
    return hit;
}

/*
 * The guest's address on a link whose host end is `host`.
 *
 * Both images put the host at .1 of the /24 and the guest at .2, so this is
 * host + 1. It is a convention rather than a reading: the guest's address is
 * configured inside the guest, and nothing on this side of the virtual wire
 * can see it until the guest sends something. Deriving it from the interface
 * at least means a renumbered link stays consistent -- move the host to
 * 10.9.0.1 and the guest is looked for at 10.9.0.2, with no code to change.
 */
static int guest_addr_from_host_addr(struct in_addr host, char *out, size_t sz)
{
    uint32_t a    = ntohl((uint32_t)host.s_addr);
    uint32_t last = a & 0xffu;

    /* .0 is the network and .254 the last usable host, so +1 has to land
       inside the subnet. Anything else means this link is not addressed the
       way the convention assumes, and guessing further would be inventing. */
    if (last == 0 || last >= 254)
        return -1;

    a = (a & 0xffffff00u) | (last + 1);

    struct in_addr guest;
    guest.s_addr = htonl(a);

    return inet_ntop(AF_INET, &guest, out, (socklen_t)sz) ? 0 : -1;
}

static int resolve_from_vdevpeer(const Guest *g, char *out, size_t sz)
{
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0)
        return -1;

    int rc = -1;
    for (struct ifaddrs *p = list; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
            continue;
        /* vdevpeer interfaces only. The bridge and the wifi are not links to
           any guest, and deriving an address off one of those would produce
           something plausible and wrong. */
        if (strncmp(p->ifa_name, "vp", 2) != 0)
            continue;
        if (!iface_peers_guest(p->ifa_name, g->system_name))
            continue;

        struct sockaddr_in *sin = (struct sockaddr_in *)(void *)p->ifa_addr;
        rc = guest_addr_from_host_addr(sin->sin_addr, out, sz);
        break;
    }

    freeifaddrs(list);
    return rc;
}

/* What the images configure, for a host whose links cannot be read. */
static const char *type_default_ip(GuestType t)
{
    switch (t) {
    case GUEST_QNX:     return "10.0.0.2";
    case GUEST_LINUX:
    case GUEST_ANDROID: return "10.0.1.2";
    default:            return NULL;
    }
}

int guest_resolve_ip(const Guest *g, char *out, size_t sz)
{
    if (!g || !out || sz == 0)
        return -1;

    out[0] = '\0';

    if (g->ip[0]) {
        snprintf(out, sz, "%s", g->ip);
        return 0;
    }

    if (resolve_from_vdevpeer(g, out, sz) == 0)
        return 0;

    const char *fallback = type_default_ip(g->type);
    if (fallback) {
        snprintf(out, sz, "%s", fallback);
        return 0;
    }

    out[0] = '\0';
    return -1;
}
