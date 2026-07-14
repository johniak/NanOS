/* resvtest — the V8 SegmentedTable memory contract: reserve a large PROT_NONE address-space region
 * without backing it with frames, then commit sub-ranges on demand via mprotect, and decommit via a
 * MAP_FIXED PROT_NONE over-map. Exercises the reserve-without-backing window (arch VA_RESV_*).
 *
 * Provably-can-fail against the OLD eager-backing model: there, mmap(PROT_NONE) routed to the 64 MiB
 * anon window and a 128 MiB request returned MAP_FAILED (window full) — so "large reservation
 * succeeds" fails — and even a smaller one that fit would DROP MemFree by the full mapped size, so
 * "reservation does not consume RAM" fails too. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>

static int fails;
#define CHECK(cond, name) do { \
    if (cond) printf("  OK  : %s\n", name); \
    else { printf("  FAIL: %s\n", name); fails++; } } while (0)

/* MemFree (KiB) from /proc/meminfo, or -1. */
static long mem_free_kb(void) {
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) return -1;
    char buf[4096]; int n = (int) read(fd, buf, sizeof buf - 1); close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    char *k = strstr(buf, "MemFree:");
    if (!k) return -1;
    return strtol(k + 8, 0, 10);
}

/* Does touching [p] fault? Fork a child that reads it; the parent survives either way. Returns 1 if
 * the child died by signal (the access faulted), 0 if it exited cleanly (the page was accessible). */
static int access_faults(volatile unsigned char *p, int write) {
    pid_t pid = fork();
    if (pid == 0) {
        if (write) *p = 0x5A; else { unsigned char v = *p; (void) v; }
        _exit(0);
    }
    int st; waitpid(pid, &st, 0);
    return WIFSIGNALED(st) ? 1 : 0;
}

int main(void) {
    printf("=== resvtest ===\n");
    const size_t RESV = (size_t) 128 * 1024 * 1024;   /* 128 MiB — bigger than the 64 MiB anon window */
    const size_t SEG  = 64 * 1024;                    /* V8's commit granularity */

    long free_before = mem_free_kb();

    /* Reserve 128 MiB with no access and no backing. */
    unsigned char *p = mmap(0, RESV, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(p != MAP_FAILED, "reserve 128 MiB PROT_NONE + MAP_NORESERVE");
    if (p == MAP_FAILED) { printf("resvtest: FAIL (%d)\n", fails + 1); return 1; }

    /* It must land in the reserve window: a HIGH VA above the identity-mapped RAM (>= 4 GiB floor),
     * NOT the low 32-bit anon window. */
    uintptr_t a = (uintptr_t) p;
    CHECK(a >= 0x100000000ull, "reservation is in the high reserve window (>= 4 GiB)");

    /* Reserving must NOT consume physical RAM (no frames backed). Allow slack for page-table frames
     * and normal churn; the point is the delta is nowhere near 128 MiB (131072 KiB). */
    long free_after = mem_free_kb();
    if (free_before > 0 && free_after > 0) {
        long drop = free_before - free_after;
        printf("  ..  : MemFree %ld -> %ld KiB (drop %ld)\n", free_before, free_after, drop);
        CHECK(drop < 8192, "reservation does not consume RAM (< 8 MiB drop, not ~128 MiB)");
    }

    /* An uncommitted page must fault on access (truly unbacked, real guard). */
    CHECK(access_faults(p + 4 * SEG, 0), "read of uncommitted reserved page faults");

    /* Commit one 64 KiB segment via mprotect(RW), then use it. */
    unsigned char *seg = p + 8 * SEG;
    CHECK(mprotect(seg, SEG, PROT_READ | PROT_WRITE) == 0, "commit a segment (mprotect RW)");
    memset(seg, 0xA5, SEG);
    int ok = 1;
    for (size_t i = 0; i < SEG; i++) if (seg[i] != 0xA5) { ok = 0; break; }
    CHECK(ok, "committed segment reads back what was written");
    CHECK(!access_faults(seg, 1), "committed segment is writable (no fault)");

    /* A DIFFERENT uncommitted segment still faults (commit didn't back the whole reservation). */
    CHECK(access_faults(p + 16 * SEG, 0), "a different segment stays uncommitted (still faults)");

    /* Decommit via MAP_FIXED PROT_NONE over-map (V8 DecommitPages) — same addr back, frames freed. */
    void *d = mmap(seg, SEG, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(d == (void *) seg, "MAP_FIXED PROT_NONE over-map returns the same addr (decommit)");
    CHECK(access_faults(seg, 0), "decommitted segment faults again");

    /* Re-commit the same VA works (frames re-allocated). */
    CHECK(mprotect(seg, SEG, PROT_READ | PROT_WRITE) == 0, "re-commit the decommitted segment");
    seg[0] = 0x11;
    CHECK(seg[0] == 0x11, "re-committed segment is usable");

    CHECK(munmap(p, RESV) == 0, "munmap the whole reservation");

    /* Teardown-with-committed-pages stress: fork children that reserve + commit several segments and
     * _exit WITHOUT munmap, so the kernel's address-space teardown must free the committed reserve-
     * window frames + on-demand page tables (freeUserWindow/freeUserTables over the HIGH VA window).
     * This is the exact path node hits when V8 aborts with pages still committed — resvtest's own
     * clean munmap above never exercises it. A double-free / wrong-PA-free would corrupt the kernel
     * heap; we assert every child exits cleanly and the parent survives many rounds. */
    int teardown_ok = 1;
    for (int i = 0; i < 40; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            unsigned char *q = mmap(0, RESV, PROT_NONE,
                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (q == MAP_FAILED) _exit(2);
            for (int k = 1; k <= 6; k++) {
                unsigned char *s2 = q + (size_t) k * 13 * SEG;
                if (mprotect(s2, SEG, PROT_READ | PROT_WRITE) != 0) _exit(3);
                s2[0] = (unsigned char) k; s2[SEG - 1] = (unsigned char) k;   // touch the committed frames
            }
            _exit(0);   /* intentionally NO munmap: exercise teardown of committed reserve pages */
        }
        int st; waitpid(pid, &st, 0);
        if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) { teardown_ok = 0; break; }
    }
    CHECK(teardown_ok, "40x reserve+commit+_exit (teardown frees committed reserve pages cleanly)");

    printf(fails ? "resvtest: FAIL (%d)\n" : "resvtest: PASS\n", fails);
    return fails ? 1 : 0;
}
