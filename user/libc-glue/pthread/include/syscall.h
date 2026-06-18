/*
 * syscall.h — NanOS adapter of musl's src/internal/syscall.h for the pthread core.
 *
 * This is a TRIMMED rewrite of musl's syscall.h, not a verbatim copy, because the upstream
 * file pulls <sys/syscall.h> (absent in picolibc) and a lot of LL/socketcall/time64 surface
 * the pthread internals never touch. We keep ONLY what __wait.c / __timedwait.c / __lock.c /
 * pthread_impl.h need:
 *   - SYSCALL_NO_TLS=1 so syscall_arch.h emits `int $128` (NanOS has no vsyscall page; the
 *     upstream default `call *%gs:16` would jump through the unused sysinfo TCB slot).
 *   - the __syscall(...) / __syscall_cp(...) variadic dispatch macros (verbatim from musl).
 *   - the Linux i386 SYS_* numbers the core references (all already NanOS syscall numbers).
 *
 * __syscall_cp(...) expands to a call of the real function __syscall_cp(); NanOS has no
 * pthread cancellation yet (Phase 5), so user/libc-glue/pthread/nanos_glue.c defines it as a
 * plain (non-cancellable) syscall. SYS_futex_time64 is intentionally undefined, so
 * __timedwait.c takes the 32-bit futex path only.
 */
#ifndef _INTERNAL_SYSCALL_H
#define _INTERNAL_SYSCALL_H

#include <errno.h>

#define SYSCALL_NO_TLS 1
#include "syscall_arch.h"

#ifndef __scc
#define __scc(X) ((long) (X))
typedef long syscall_arg_t;
#endif

hidden long __syscall_ret(unsigned long);
hidden long __syscall_cp(syscall_arg_t, syscall_arg_t, syscall_arg_t, syscall_arg_t,
                         syscall_arg_t, syscall_arg_t, syscall_arg_t);

#define __syscall1(n,a) __syscall1(n,__scc(a))
#define __syscall2(n,a,b) __syscall2(n,__scc(a),__scc(b))
#define __syscall3(n,a,b,c) __syscall3(n,__scc(a),__scc(b),__scc(c))
#define __syscall4(n,a,b,c,d) __syscall4(n,__scc(a),__scc(b),__scc(c),__scc(d))
#define __syscall5(n,a,b,c,d,e) __syscall5(n,__scc(a),__scc(b),__scc(c),__scc(d),__scc(e))
#define __syscall6(n,a,b,c,d,e,f) __syscall6(n,__scc(a),__scc(b),__scc(c),__scc(d),__scc(e),__scc(f))
#define __syscall7(n,a,b,c,d,e,f,g) __syscall7(n,__scc(a),__scc(b),__scc(c),__scc(d),__scc(e),__scc(f),__scc(g))

#define __SYSCALL_NARGS_X(a,b,c,d,e,f,g,h,n,...) n
#define __SYSCALL_NARGS(...) __SYSCALL_NARGS_X(__VA_ARGS__,7,6,5,4,3,2,1,0,)
#define __SYSCALL_CONCAT_X(a,b) a##b
#define __SYSCALL_CONCAT(a,b) __SYSCALL_CONCAT_X(a,b)
#define __SYSCALL_DISP(b,...) __SYSCALL_CONCAT(b,__SYSCALL_NARGS(__VA_ARGS__))(__VA_ARGS__)

#define __syscall(...) __SYSCALL_DISP(__syscall,__VA_ARGS__)
#define syscall(...) __syscall_ret(__syscall(__VA_ARGS__))

#define __syscall_cp0(n) (__syscall_cp)(n,0,0,0,0,0,0)
#define __syscall_cp1(n,a) (__syscall_cp)(n,__scc(a),0,0,0,0,0)
#define __syscall_cp2(n,a,b) (__syscall_cp)(n,__scc(a),__scc(b),0,0,0,0)
#define __syscall_cp3(n,a,b,c) (__syscall_cp)(n,__scc(a),__scc(b),__scc(c),0,0,0)
#define __syscall_cp4(n,a,b,c,d) (__syscall_cp)(n,__scc(a),__scc(b),__scc(c),__scc(d),0,0)
#define __syscall_cp5(n,a,b,c,d,e) (__syscall_cp)(n,__scc(a),__scc(b),__scc(c),__scc(d),__scc(e),0)
#define __syscall_cp6(n,a,b,c,d,e,f) (__syscall_cp)(n,__scc(a),__scc(b),__scc(c),__scc(d),__scc(e),__scc(f))

#define __syscall_cp(...) __SYSCALL_DISP(__syscall_cp,__VA_ARGS__)
#define syscall_cp(...) __syscall_ret(__syscall_cp(__VA_ARGS__))

/* Syscall numbers used by the vendored pthread core (== NanOS syscall numbers, kernel/
 * SyscallNr.h). Arch-selected: x86_64 (the `syscall` insn path) uses the Linux x86_64 numbers;
 * i386 (`int $128`) keeps the Linux i386 numbers verbatim. SYS_exit in particular MUST be the
 * right number — pthread_create.c's thread-exit issues __syscall(SYS_exit, 0), which is 60 on
 * x86_64 (1 there is write(2)). clone/set_thread_area are issued from the arch .s only. */
#if defined(__x86_64__)
#define SYS_exit            60
#define SYS_munmap          11
#define SYS_clone           56
#define SYS_rt_sigaction    13
#define SYS_sched_yield     24
#define SYS_mmap            9
#define SYS_gettid          186
#define SYS_tkill           200
#define SYS_futex           202
#define SYS_set_thread_area 205   /* unused on x86_64 (TLS via arch_prctl); kept for completeness */
/* Compile-time link-completeness only (robust-mutex path); NanOS implements no robust list. */
#define SYS_set_robust_list 273
#else
#define SYS_exit            1
#define SYS_munmap          91
#define SYS_clone           120
#define SYS_rt_sigaction    174
#define SYS_sched_yield     158
#define SYS_mmap2           192
#define SYS_gettid          224
#define SYS_tkill           238
#define SYS_futex           240
#define SYS_set_thread_area 243
/* Task 4.3: referenced (compile-time) by the robust-mutex registration path in
 * pthread_mutex_trylock.c. NanOS implements no robust-futex list, but a process-private,
 * non-robust mutex never reaches that path, so this is link-completeness only — if it ever
 * executed, the kernel would return -ENOSYS for the unknown number. */
#define SYS_set_robust_list 311
#endif

#endif
