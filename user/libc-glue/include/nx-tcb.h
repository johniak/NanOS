/*
 * nx-tcb.h — the per-thread Thread Control Block (TCB) for the NanOS userland.
 *
 * i386 TLS is "variant II": the thread pointer (here %gs, GDT entry 6 / selector 0x33,
 * pointed at this block by set_thread_area(2)) addresses the TCB itself, and its FIRST
 * word is `self` (a pointer to the TCB). So `%gs:0` yields the TCB address — that is how
 * __pthread_self() recovers the running thread's control block with no syscall.
 *
 * The layout deliberately matches musl's i386 `struct pthread` for its ABI ("Part 1")
 * prefix so that Phase 4 (vendoring musl's pthread) can drop in with the same field
 * offsets. The fields the libc cares about today:
 *     offset 0   self      thread pointer (== &tcb)         [ABI: read via %gs:0]
 *     offset 24  tid       kernel thread id
 *     offset 28  __errno   the per-thread errno cell        [stable, documented offset]
 * Everything past __errno is reserved for musl's Part-2 fields (cancel/join/detach,
 * tsd, locale, robust-list, ...) added in Phase 4; the reserve keeps those addresses
 * inside the allocated block so early code that only needs errno still works.
 */
#ifndef NX_TCB_H
#define NX_TCB_H

struct __pthread {
	struct __pthread *self;     /* 0  : == &this; the i386 thread pointer (%gs:0) */
	void             *dtv;      /* 4  : dynamic thread vector (unused until TLS data) */
	struct __pthread *prev;     /* 8  : thread list links (musl ABI slots) */
	struct __pthread *next;     /* 12 */
	unsigned long     sysinfo;  /* 16 : vDSO entry on Linux; unused here */
	unsigned long     canary;   /* 20 : stack-protector canary */
	/* Part 2 — implementation detail. errno lives at a FIXED offset (28). */
	int               tid;      /* 24 : kernel tid (set by gettid / clone) */
	int               __errno;  /* 28 : the per-thread errno cell */
	/* Reserve room for musl's remaining pthread fields, populated in Phase 4. */
	char              __reserved[256];
};

/* The running thread's TCB: i386 reads it from the thread pointer at %gs:0. Valid only
 * after set_thread_area has installed the TLS descriptor (see __nx_init_tls / pthread
 * creation); callers in the startup path must run AFTER that. */
static inline struct __pthread *__pthread_self(void) {
	struct __pthread *self;
	__asm__("mov %%gs:0,%0" : "=r"(self));
	return self;
}

#endif /* NX_TCB_H */
