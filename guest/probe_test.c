/*
 * Self-check for port_open(), the reachability probe that replaced the
 * ssh-based one in discoverer.c.
 *
 * It matters that this is both correct AND bounded: it runs once per guest per
 * refresh cycle, so a probe that blocks on an unroutable address would stall
 * the loop in exactly the way the ssh probe used to.
 *
 * discoverer.c is #included so the real static function is tested rather than
 * a copy.
 *
 *   gcc -O1 -I../stub -o probe_test guest/probe_test.c <other sources> && ./probe_test
 */
#include "discoverer.c"

#include <assert.h>
#include <sys/time.h>

static long ms_since(struct timeval *t0)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec - t0->tv_sec) * 1000 + (now.tv_usec - t0->tv_usec) / 1000;
}

int main(void)
{
    /* A listening socket on a port the OS picks, so the test cannot collide
       with whatever else is running on this machine. */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    assert(srv >= 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    assert(bind(srv, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    assert(listen(srv, 4) == 0);

    socklen_t slen = sizeof(sa);
    assert(getsockname(srv, (struct sockaddr *)&sa, &slen) == 0);
    int open_port = ntohs(sa.sin_port);

    /* 1. A port that is listening. */
    assert(port_open("127.0.0.1", open_port, 400) == 1);

    /* 2. The same port once nothing is listening: must be refused, not "open". */
    close(srv);
    assert(port_open("127.0.0.1", open_port, 400) == 0);

    /* 3. Garbage inputs must not be reported as reachable. */
    assert(port_open(NULL, 22, 400) == 0);
    assert(port_open("", 22, 400) == 0);
    assert(port_open("not-an-ip", 22, 400) == 0);
    assert(port_open("10.0.0.2", 0, 400) == 0);

    /* 4. The one that actually protects the refresh loop: an address nothing
          answers for must give up at the timeout, NOT sit through the kernel's
          multi-second SYN retries. 192.0.2.0/24 is TEST-NET-1, reserved and
          unroutable by definition. */
    struct timeval t0;
    gettimeofday(&t0, NULL);
    int r = port_open("192.0.2.1", 22, 400);
    long took = ms_since(&t0);

    assert(r == 0);
    /* Generous upper bound -- the point is "bounded by the timeout", not the
       exact scheduling latency. Without the non-blocking connect this takes
       tens of seconds. */
    assert(took < 2000);

    printf("port_open: listening=ok refused=ok garbage=ok "
           "unroutable=%ldms (bounded)\n", took);
    printf("probe_test: OK\n");
    return 0;
}
