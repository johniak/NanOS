/*
 * nanos-node-compat.h — force-included (-include) into the Node/V8 cross build for a handful of
 * constants that live in glibc's <signal.h> siginfo section but not picolibc's. V8's crash handler
 * (base/debug/stack_trace_posix.cc) switches on these si_code values to pretty-print a fault. They
 * are plain integers with the standard Linux x86 values; #ifndef-guarded so a future signal.h that
 * defines them wins. Kept out of the sysroot signal.h itself so we don't fork picolibc's header.
 */
#ifndef NANOS_NODE_COMPAT_H
#define NANOS_NODE_COMPAT_H

/* BSD <sys/param.h> rounding macros picolibc omits (postject / SEA uses roundup). Force-included so
 * they're defined before the consumer includes <sys/param.h>; #ifndef-guarded so a header that does
 * define them wins. */
#ifndef roundup
#define roundup(x, y)   ((((x) + ((y) - 1)) / (y)) * (y))
#endif
#ifndef rounddown
#define rounddown(x, y) (((x) / (y)) * (y))
#endif
#ifndef powerof2
#define powerof2(x)     ((((x) - 1) & (x)) == 0)
#endif

/* SIGSEGV */
#ifndef SEGV_MAPERR
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#endif
/* SIGBUS */
#ifndef BUS_ADRALN
#define BUS_ADRALN 1
#define BUS_ADRERR 2
#define BUS_OBJERR 3
#endif
/* SIGFPE */
#ifndef FPE_INTDIV
#define FPE_INTDIV 1
#define FPE_INTOVF 2
#define FPE_FLTDIV 3
#define FPE_FLTOVF 4
#define FPE_FLTUND 5
#define FPE_FLTRES 6
#define FPE_FLTINV 7
#define FPE_FLTSUB 8
#endif
/* SIGILL */
#ifndef ILL_ILLOPC
#define ILL_ILLOPC 1
#define ILL_ILLOPN 2
#define ILL_ILLADR 3
#define ILL_ILLTRP 4
#define ILL_PRVOPC 5
#define ILL_PRVREG 6
#define ILL_COPROC 7
#define ILL_BADSTK 8
#endif

#endif /* NANOS_NODE_COMPAT_H */
