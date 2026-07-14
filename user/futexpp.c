/* futexpp — SMP futex lost-wakeup gate. Two threads ping-pong a token through a pthread condition
 * variable: each round one thread cond_wait()s (kernel FUTEX_WAIT) until it is its turn, the other
 * cond_signal()s (kernel FUTEX_WAKE). If a FUTEX_WAKE on one CPU can dequeue a waiter and wake it
 * before the waiter has transitioned to TASK_BLOCKED (Scheduler::wake is a no-op on a not-yet-blocked
 * task), the wakeup is LOST and that thread parks forever -> the ping-pong deadlocks and never prints
 * PASS. That is exactly the race that hung V8/node's worker<->main handshake at -smp>1 while single
 * core worked. This must run at -smp 4 to expose it.
 *
 * Provably-can-fail: revert the futex WAIT park to "enqueue, unlock g_futexLock, THEN block()" and
 * this hangs (no PASS) at -smp 4. With the BLOCKED flip done under g_futexLock it completes.
 */
#include <stdio.h>
#include <pthread.h>

#define ITERS 100000

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  c = PTHREAD_COND_INITIALIZER;
static volatile int turn = 0;         /* whose turn it is: 0 or 1 */

static void *side(void *arg) {
    long me = (long) arg;             /* 0 or 1 */
    for (int i = 0; i < ITERS; i++) {
        pthread_mutex_lock(&m);
        while (turn != me)
            pthread_cond_wait(&c, &m);        /* FUTEX_WAIT — the side that must not lose a wakeup */
        turn = 1 - (int) me;
        pthread_cond_signal(&c);              /* FUTEX_WAKE the peer */
        pthread_mutex_unlock(&m);
    }
    return 0;
}

int main(void) {
    printf("=== futexpp ===\n");
    pthread_t a, b;
    if (pthread_create(&a, 0, side, (void *) 0) || pthread_create(&b, 0, side, (void *) 1)) {
        printf("futexpp: FAIL (create)\n"); return 1;
    }
    pthread_join(a, 0);               /* if a wakeup was lost, one side parks forever and join hangs */
    pthread_join(b, 0);
    printf("futexpp: PASS (%d handshakes)\n", ITERS);
    return 0;
}
