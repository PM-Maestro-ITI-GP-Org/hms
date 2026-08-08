/*
 * Self-check for the ssh concurrency gate in client.c.
 *
 * The gate exists because more than two ssh handshakes in flight at once make
 * every one of them fail (see the SSH_MAX_INFLIGHT comment). That is a claim
 * about a counter shared by five threads, so it gets one runnable check rather
 * than a code review.
 *
 * client.c is #included, not linked, so the test drives the real static
 * ssh_gate_enter/ssh_gate_leave instead of a copy that could drift from them.
 *
 * Build and run (host gcc, not qcc -- this tests logic, not the target):
 *
 *     gcc -fsanitize=thread -O1 -o gate_test ssh/gate_test.c && ./gate_test
 */
#include "client.c"

#include <assert.h>

#define THREADS 8
#define ROUNDS  200

static pthread_mutex_t obs_lock = PTHREAD_MUTEX_INITIALIZER;
static int obs_now;   /* how many are inside the gate right now */
static int obs_max;   /* the high-water mark, which is what we assert on */

static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < ROUNDS; i++) {
        ssh_gate_enter();

        pthread_mutex_lock(&obs_lock);
        obs_now++;
        if (obs_now > obs_max) obs_max = obs_now;
        pthread_mutex_unlock(&obs_lock);

        /* Stand in for the ~1.9s a real handshake costs, scaled down. */
        struct timespec ts = { 0, 200000 };  /* 0.2 ms */
        nanosleep(&ts, NULL);

        pthread_mutex_lock(&obs_lock);
        obs_now--;
        pthread_mutex_unlock(&obs_lock);

        ssh_gate_leave();
    }
    return NULL;
}

int main(void)
{
    pthread_t t[THREADS];

    for (int i = 0; i < THREADS; i++)
        assert(pthread_create(&t[i], NULL, worker, NULL) == 0);
    for (int i = 0; i < THREADS; i++)
        assert(pthread_join(t[i], NULL) == 0);

    printf("threads=%d rounds=%d limit=%d observed_max=%d\n",
           THREADS, ROUNDS, SSH_MAX_INFLIGHT, obs_max);

    /* The point of the gate: never more than the limit at once. */
    assert(obs_max <= SSH_MAX_INFLIGHT);

    /* And it must not have over-serialised into a limit of one, which would
       still pass the check above while halving throughput for no reason. */
    assert(obs_max == SSH_MAX_INFLIGHT);

    /* Every enter released its slot, so the counter is back to zero. */
    assert(ssh_inflight == 0);
    assert(obs_now == 0);

    printf("gate_test: OK\n");
    return 0;
}
