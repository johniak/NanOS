/*
 * pthread_arch.h — NanOS thread-pointer override for the vendored musl pthread core.
 *
 * Both NanOS x86 arches use TLS "variant II": the thread pointer addresses the START of
 * the TCB, whose FIRST word is `self`, so `<tp>:0` reads `self`. Neither arch defines
 * TLS_ABOVE_TP, so musl's default arithmetic in pthread_impl.h (TP_ADJ(p)==p,
 * __pthread_self()==__get_tp()) is the correct one for both. Per-arch the thread pointer
 * and its installation differ; the TP-to-pthread arithmetic does not.
 *
 *   - x86_64 : the thread pointer is %fs.base (installed via arch_prctl ARCH_SET_FS, see
 *              Task 3); __get_tp() reads %fs:0. Mirrors musl arch/x86_64/pthread_arch.h.
 *              TP_ADJ / __pthread_self come from pthread_impl.h's default
 *              `#ifndef TLS_ABOVE_TP` branch (== identity), exactly as upstream x86_64.
 *   - i386   : the thread pointer is %gs (GDT entry 6 / selector 0x33, installed via
 *              set_thread_area(2), Task 3.1); __get_tp() reads %gs:0. musl i386 is NOT
 *              TLS_ABOVE_TP either, so the thread pointer IS the struct pthread base — no
 *              negative-offset variant-II adjustment. We restate TP_ADJ / __pthread_self
 *              here (textually identical to pthread_impl.h's branch, so an identical and
 *              therefore legal redefinition) to make the NanOS contract self-documenting.
 *
 * Consequence (both): musl's struct pthread Part-1 fields sit at <tp>+0.. — self@0, dtv,
 * prev, next, sysinfo, canary, tid, errno_val — laid out IDENTICALLY to the picolibc-glue
 * view in user/libc-glue/include/nx-tcb.h (widths scale with the arch's pointer size). So
 * musl's errno and picolibc's errno (via __errno_location in tls.c) are the SAME cell.
 */
#if defined(__x86_64__)
static inline uintptr_t __get_tp()
{
	uintptr_t tp;
	__asm__ ("mov %%fs:0,%0" : "=r" (tp) );
	return tp;
}

#define MC_PC gregs[REG_RIP]
#else
static inline uintptr_t __get_tp()
{
	uintptr_t tp;
	__asm__ ("movl %%gs:0,%0" : "=r" (tp) );
	return tp;
}

#define TP_ADJ(p) (p)
#define __pthread_self() ((pthread_t)__get_tp())

#define MC_PC gregs[REG_EIP]
#endif
