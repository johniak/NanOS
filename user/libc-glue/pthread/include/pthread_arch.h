/*
 * pthread_arch.h — NanOS i386 thread-pointer override for the vendored musl pthread core.
 *
 * NanOS's per-thread TLS (Task 3.1) installs the i386 thread pointer (%gs, GDT entry 6 /
 * selector 0x33) pointing at the START of the TCB, so `%gs:0` reads `self`. This matches
 * musl's i386 model exactly: musl i386 is NOT TLS_ABOVE_TP, so its thread pointer also IS
 * the struct base. We therefore reuse Task 3.1 verbatim — no negative-offset variant-II
 * adjustment.
 *
 *   - __get_tp()        reads %gs:0  (the TCB base == self)
 *   - TP_ADJ(p)         identity     (the thread pointer IS the struct pthread base)
 *   - __pthread_self()  reads %gs:0  (no subtraction of sizeof(struct pthread))
 *
 * The TP_ADJ / __pthread_self definitions below are textually IDENTICAL to the
 * `#ifndef TLS_ABOVE_TP` branch in pthread_impl.h, so vendoring pthread_impl.h verbatim
 * causes no macro conflict (an identical redefinition is allowed). We define them here too
 * to make the NanOS contract explicit and self-documenting, per the integration design.
 *
 * Consequence: musl's struct pthread Part-1 fields sit at %gs+0..28 — self@0, dtv@4,
 * prev@8, next@12, sysinfo@16, canary@20, tid@24, errno_val@28 — IDENTICAL to the
 * picolibc-glue view in user/libc-glue/include/nx-tcb.h. errno_val@28 == nx-tcb.h __errno@28,
 * so musl's errno and picolibc's errno (via __errno_location in tls.c) are the SAME cell.
 */
static inline uintptr_t __get_tp()
{
	uintptr_t tp;
	__asm__ ("movl %%gs:0,%0" : "=r" (tp) );
	return tp;
}

#define TP_ADJ(p) (p)
#define __pthread_self() ((pthread_t)__get_tp())

#define MC_PC gregs[REG_EIP]
