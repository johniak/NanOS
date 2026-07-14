/* mallocstorm — prove sbrk() batches the kernel brk syscall instead of firing one per allocation.
 *
 * NanOS's picolibc nano-malloc calls sbrk() once per un-satisfiable allocation. If sbrk maps 1:1 to
 * the brk(2) syscall (the old behaviour), a malloc-heavy startup like V8/node pays one kernel brk per
 * malloc — and on NanOS each brk eagerly maps+zeroes frames AND does two CR3 reloads (a full TLB
 * flush), so tens of thousands of them dominate startup. With batching, sbrk grows the kernel break
 * geometrically and hands out sub-slices with NO syscall, so a storm of small mallocs costs only a
 * handful of brk syscalls total.
 *
 * The test is SELF-CALIBRATING (robust to TCG/host speed): it times N pure getpid() syscalls as a
 * baseline, then times N growing malloc()s. If every malloc still did a brk syscall, the storm would
 * be at LEAST as slow as the getpid baseline (brk is heavier than getpid). Batching makes the storm
 * mostly userland bookkeeping -> many times faster than the syscall baseline.
 *
 * Provably-can-fail: revert sbrk to the 1:1 brk mapping and the storm is >= the baseline -> FAIL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>

#define N 100000

static void *slots[N];

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

int main(void) {
    printf("=== mallocstorm ===\n");

    /* Baseline: N pure syscalls (getpid can't be served from userland -> one trap each). */
    volatile long sink = 0;
    uint64_t t0 = now_ns();
    for (int i = 0; i < N; i++) sink += getpid();
    uint64_t t1 = now_ns();
    uint64_t baseline = t1 - t0;

    /* Storm: N growing allocations, none freed, so nano-malloc must extend the heap each time. Touch
     * the first byte so the allocation can't be optimized away and is really backed. */
    uint64_t t2 = now_ns();
    int failed_alloc = 0;
    for (int i = 0; i < N; i++) {
        void *p = malloc(24);
        if (!p) { failed_alloc = 1; slots[i] = 0; continue; }
        *(volatile char *) p = (char) i;
        slots[i] = p;
    }
    uint64_t t3 = now_ns();
    uint64_t storm = t3 - t2;

    /* Keep the allocations reachable past the timing (defeat DCE) and free them. */
    unsigned long checksum = 0;
    for (int i = 0; i < N; i++) if (slots[i]) checksum += (unsigned char) *(volatile char *) slots[i];
    for (int i = 0; i < N; i++) free(slots[i]);

    printf("  baseline %d getpid : %llu ns\n", N, (unsigned long long) baseline);
    printf("  storm    %d malloc : %llu ns\n", N, (unsigned long long) storm);
    printf("  checksum=%lu sink=%ld alloc_fail=%d\n", checksum, (long) sink, failed_alloc);

    /* Batched sbrk => the storm is mostly userland => comfortably faster than N raw syscalls.
     * Require a 2x margin so the verdict is unambiguous across TCG timing jitter. A 1:1 sbrk->brk
     * mapping makes the storm >= the baseline and this fails. */
    int pass = !failed_alloc && baseline > 0 && storm * 2 < baseline;
    printf(pass ? "mallocstorm: PASS\n" : "mallocstorm: FAIL\n");
    return pass ? 0 : 1;
}
