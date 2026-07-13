/*
 * sys/ucontext.h — the x86_64 machine-context layout, glibc-compatible, for ports that read a signal
 * handler's ucontext (abseil/V8 extract the faulting PC for crash backtraces via
 * uc_mcontext.gregs[REG_RIP]). Only the register-file layout matters here; NanOS crash backtraces are
 * best-effort. This mirrors glibc's <sys/ucontext.h> so `gregs[16]` is RIP.
 */
#ifndef _SYS_UCONTEXT_H
#define _SYS_UCONTEXT_H

#include <signal.h>   /* stack_t, sigset_t */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef long long greg_t;
#define NGREG 23
typedef greg_t gregset_t[NGREG];

/* glibc register indices (x86_64). REG_RIP == 16 is the one abseil/V8 use. */
enum {
	REG_R8 = 0, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15,
	REG_RDI, REG_RSI, REG_RBP, REG_RBX, REG_RDX, REG_RAX, REG_RCX, REG_RSP,
	REG_RIP, REG_EFL, REG_CSGSFS, REG_ERR, REG_TRAPNO, REG_OLDMASK, REG_CR2
};

struct _libc_fpstate;   /* opaque FP save area (not modelled) */

typedef struct {
	gregset_t gregs;
	struct _libc_fpstate *fpregs;
	unsigned long long __reserved1[8];
} mcontext_t;

typedef struct ucontext_t {
	unsigned long uc_flags;
	struct ucontext_t *uc_link;
	stack_t uc_stack;
	mcontext_t uc_mcontext;
	sigset_t uc_sigmask;
	unsigned long __fpregs_mem[64];
} ucontext_t;

#ifdef __cplusplus
}
#endif

#endif /* _SYS_UCONTEXT_H */
