/*
 * ucontext.h — the machine context type (via <sys/ucontext.h>) plus the SysV context-switch API.
 * Ports include this for `ucontext_t` when reading a signal context (abseil/V8 stack tracing). NanOS
 * has no swapcontext-style user threading (threads are pthreads), so the getcontext/makecontext
 * family are declared for source compatibility and return -ENOSYS at runtime (libc-glue).
 */
#ifndef _UCONTEXT_H
#define _UCONTEXT_H

#include <sys/ucontext.h>

#ifdef __cplusplus
extern "C" {
#endif

int  getcontext(ucontext_t *ucp);
int  setcontext(const ucontext_t *ucp);
void makecontext(ucontext_t *ucp, void (*func)(void), int argc, ...);
int  swapcontext(ucontext_t *oucp, const ucontext_t *ucp);

#ifdef __cplusplus
}
#endif

#endif /* _UCONTEXT_H */
