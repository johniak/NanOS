/*
 * fputorture — the FPU/SSE context-switch integrity gate.
 *
 * The kernel enables SSE for ring 3 but archContextSwitch historically saved only the
 * callee-saved GPRs + CR3 — XMM0-15/MXCSR were NEVER switched. A deferred preemption lands
 * mid-computation (ret-to-ring3, not a call boundary), so a resumed process silently continued
 * with ANOTHER process's registers: random data corruption that presented on the Dell as
 * "impossible" SIGSEGVs inside a fuzz-proven PNG decoder under boot-time context-switch storm.
 *
 * This program is the direct oracle: NPROC forked workers each park a UNIQUE pattern in
 * xmm8..xmm11 and a unique rounding-mode in MXCSR, busy-spin long enough to guarantee timer
 * preemptions land inside the window, then read the registers back. The generated loop code is
 * pure integer (no calls between park and check), so by the SysV ABI nothing in THIS process
 * may touch those registers — any change means the kernel leaked another task's state in.
 *
 *   PASS line (the smoke greps for it): "FPUTORTURE PASS leaks=0"
 *   Any leak:                           "FPUTORTURE LEAK ..." + nonzero total.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define NPROC 8       /* MORE workers than CPUs (-smp 4) so workers preempt EACH OTHER —
                       * equal counts would pin one per core and mostly switch against idle
                       * tasks, which never touch XMM (the kernel is -mno-sse) */
#define ITERS 12000
#define SPIN  20000   /* busy window per iteration — wide enough for ~ms-scale preemption */

/* Park pattern quads in xmm8..xmm11 (callee-untouched by this loop: no calls, no FP codegen).
 * NOTE the "m"(v) input: the asm reads *v, and with only the POINTER as input GCC is free to
 * treat the v[] stores as dead and elide them — the first version did exactly that and loaded
 * stale stack bytes, which (being fork-copied) were IDENTICAL in every worker and mimicked a
 * kernel leak with a constant value. */
#define PARK1(reg, e0, e1) do { \
	v[0] = (e0); v[1] = (e1); \
	__asm__ __volatile__("movdqu %1, %%" reg : : "r"(v), "m"(v) : reg); \
} while (0)

static inline void park(unsigned long long a, unsigned long long b)
{
	unsigned long long v[2];
	PARK1("xmm8",  a, b);
	PARK1("xmm9",  a ^ 0x1111111111111111ull, b ^ 0x2222222222222222ull);
	PARK1("xmm10", a ^ 0x3333333333333333ull, b ^ 0x4444444444444444ull);
	PARK1("xmm11", a ^ 0x5555555555555555ull, b ^ 0x6666666666666666ull);
}

static unsigned long long g_seen0, g_seen1;   /* xmm8 as observed by the last failing check */

static inline int check(unsigned long long a, unsigned long long b)
{
	unsigned long long v[2], want0, want1;
	int bad = 0;
	__asm__ __volatile__("movdqu %%xmm8, (%0)"  : : "r"(v) : "memory");
	if (v[0] != a || v[1] != b) { bad |= 1; g_seen0 = v[0]; g_seen1 = v[1]; }
	__asm__ __volatile__("movdqu %%xmm9, (%0)"  : : "r"(v) : "memory");
	want0 = a ^ 0x1111111111111111ull; want1 = b ^ 0x2222222222222222ull;
	if (v[0] != want0 || v[1] != want1) bad |= 2;
	__asm__ __volatile__("movdqu %%xmm10, (%0)" : : "r"(v) : "memory");
	want0 = a ^ 0x3333333333333333ull; want1 = b ^ 0x4444444444444444ull;
	if (v[0] != want0 || v[1] != want1) bad |= 4;
	__asm__ __volatile__("movdqu %%xmm11, (%0)" : : "r"(v) : "memory");
	want0 = a ^ 0x5555555555555555ull; want1 = b ^ 0x6666666666666666ull;
	if (v[0] != want0 || v[1] != want1) bad |= 8;
	return bad;
}

static int worker(int idx)
{
	/* Unique per-worker MXCSR rounding-control (bits 13-14) on top of all-masked 0x1F80:
	 * a leak of another worker's MXCSR flips RC — checked every iteration. */
	unsigned mx = 0x1F80u | ((unsigned) idx << 13);
	unsigned mxrd;
	unsigned long long leaks = 0;
	unsigned long long base = 0x1000100010001000ull * (unsigned long long) (idx + 1);
	__asm__ __volatile__("ldmxcsr %0" : : "m"(mx));
	for (int it = 0; it < ITERS; it++) {
		unsigned long long a = base + (unsigned long long) it;
		unsigned long long b = ~a;
		park(a, b);
		for (volatile int s = 0; s < SPIN; s++) { }   /* preemption window */
		int bad = check(a, b);
		__asm__ __volatile__("stmxcsr %0" : "=m"(mxrd));
		if (mxrd != mx) bad |= 16;
		if (bad) {
			leaks++;
			if (leaks > 8) continue;   /* print the first few in full, then just count */
			printf("FPUTORTURE LEAK worker=%d iter=%d bad=0x%x mxcsr=0x%x want=0x%x seen=%llx/%llx want=%llx/%llx\n",
			       idx, it, bad, mxrd, mx, g_seen0, g_seen1, a, ~a);
			__asm__ __volatile__("ldmxcsr %0" : : "m"(mx));   /* re-arm and keep counting */
		}
	}
	return leaks > 200 ? 200 : (int) leaks;   /* exit code caps at 200 (wait status is 8-bit) */
}

int main(void)
{
	int pids[NPROC], total = 0;
	printf("fputorture: %d workers x %d iters (xmm8-11 + MXCSR integrity under preemption)\n",
	       NPROC, ITERS);
	for (int i = 0; i < NPROC; i++) {
		int pid = fork();
		if (pid == 0)
			_exit(worker(i));
		pids[i] = pid;
	}
	for (int i = 0; i < NPROC; i++) {
		int st = 0;
		waitpid(pids[i], &st, 0);
		if (WIFEXITED(st))
			total += WEXITSTATUS(st);
		else
			total += 200;   /* a crashed worker IS a failure (wild pointer from a leak) */
	}
	if (total == 0)
		printf("FPUTORTURE PASS leaks=0\n");
	else
		printf("FPUTORTURE FAIL leaks=%d\n", total);
	return total ? 1 : 0;
}
