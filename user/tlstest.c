/* tlstest — per-thread TLS on WORKER threads (pthread_create's __copy_tls path). NanOS worker
 * threads historically got a bare TCB with no __thread block; node/V8 is the first program with real
 * thread_locals on worker threads (e.g. V8's non-zero-initialised assert-scope data). This verifies:
 *   1. a __thread var with a NON-ZERO initializer reads that initializer on a fresh worker (the
 *      .tdata template is copied, not zeroed — the exact failure behind V8's AllowHeapAllocation CHECK);
 *   2. each thread's __thread storage is PRIVATE (writes don't bleed across threads), under contention.
 *
 * Provably-can-fail: with the old bare-TCB pthread_create, worker threads share/garble %fs-relative
 * TLS — the initializer reads 0 and/or threads clobber each other, so a CHECK below fails. */
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <sched.h>

#define NT 8

/* Non-zero initializer -> lands in .tdata (like V8's current_per_thread_assert_data = 0xFFFFFFDF).
 * A zeroed TLS block would read 0 here. Volatile so the compiler can't fold the check away. */
static __thread volatile unsigned tls_magic = 0xABCD1234u;
static __thread volatile int      tls_slot   = -1;   /* .tdata too (non-zero init) */

static int fails;
static pthread_mutex_t iolock = PTHREAD_MUTEX_INITIALIZER;
static volatile int arrived;   /* hand-rolled barrier (pthread_barrier may be absent in the port) */

static void report(const char *name, int ok) {
    pthread_mutex_lock(&iolock);
    if (ok) printf("  OK  : %s\n", name);
    else { printf("  FAIL: %s\n", name); fails++; }
    pthread_mutex_unlock(&iolock);
}

static void barrier_wait(void) {
    __atomic_add_fetch(&arrived, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&arrived, __ATOMIC_SEQ_CST) < NT)
        sched_yield();
}

static void *worker(void *a) {
    long id = (long) a;
    /* Fresh worker: the __thread vars must hold their INITIALIZERS, not zero. */
    report("worker sees tls_magic initializer (0xABCD1234)", tls_magic == 0xABCD1234u);
    report("worker sees tls_slot initializer (-1)", tls_slot == -1);

    /* Each thread stamps its own id, then all threads rendezvous so their writes overlap in time,
     * then each re-reads: private TLS => everyone still sees their own id. */
    tls_slot  = (int) id;
    tls_magic = 0x1000u + (unsigned) id;
    barrier_wait();
    report("tls_slot stayed private across threads", tls_slot == (int) id);
    report("tls_magic stayed private across threads", tls_magic == 0x1000u + (unsigned) id);
    return 0;
}

int main(void) {
    printf("=== tlstest ===\n");
    pthread_t t[NT];
    for (long i = 0; i < NT; i++)
        if (pthread_create(&t[i], 0, worker, (void *) i) != 0) {
            printf("tlstest: FAIL (create)\n"); return 1;
        }
    for (int i = 0; i < NT; i++) pthread_join(t[i], 0);

    /* The main thread's own TLS is unaffected by the workers. */
    report("main thread tls_magic intact", tls_magic == 0xABCD1234u);

    printf(fails ? "tlstest: FAIL (%d)\n" : "tlstest: PASS\n", fails);
    return fails ? 1 : 0;
}
