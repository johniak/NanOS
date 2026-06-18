/*
 * nx-tcb.h — the per-thread Thread Control Block (TCB) for the NanOS userland.
 *
 * Both x86 arches use TLS "variant II": the thread pointer addresses the TCB itself, and
 * its FIRST word is `self` (a pointer to the TCB). So `<tp>:0` yields the TCB address —
 * that is how __pthread_self() recovers the running thread's control block with no syscall.
 *
 *   - x86_64 : thread pointer is %fs.base (installed via arch_prctl(ARCH_SET_FS, &tcb),
 *              Task 3); %fs:0 reads `self`. Pointers and uintptr-width fields are 8 bytes.
 *   - i386   : thread pointer is %gs (GDT entry 6 / selector 0x33, installed via
 *              set_thread_area(2)); %gs:0 reads `self`. Those fields are 4 bytes.
 *
 * The layout deliberately matches musl's `struct pthread` ABI ("Part 1") prefix so that
 * the vendored musl pthread core drops in with the same field offsets. errno lives at a
 * FIXED field (`__errno`, == musl's Part-2 `errno_val`); __errno_location() in tls.c hands
 * out &__pthread_self()->__errno, so musl's errno and picolibc's errno are the SAME cell.
 * Everything past __errno is reserved for musl's remaining Part-2 fields.
 *
 *   x86_64 offsets:  self@0  dtv@8  prev@16 next@24 sysinfo@32 canary@40 tid@48 __errno@52
 *   i386   offsets:  self@0  dtv@4  prev@8  next@12 sysinfo@16 canary@20 tid@24 __errno@28
 *
 * (The C field types — pointers + `unsigned long` + `int` — scale to the arch's word size
 * automatically; the per-arch split below makes the resulting widths/offsets explicit and
 * keeps the thread-pointer asm arch-correct.)
 */
#ifndef NX_TCB_H
#define NX_TCB_H

#if defined(__x86_64__)

struct __pthread {
	struct __pthread *self;     /* 0  : == &this; the x86_64 thread pointer (%fs:0)   [8B] */
	void             *dtv;      /* 8  : dynamic thread vector (unused until TLS data)  [8B] */
	struct __pthread *prev;     /* 16 : thread list links (musl ABI slots)            [8B] */
	struct __pthread *next;     /* 24                                                 [8B] */
	unsigned long     sysinfo;  /* 32 : vDSO entry on Linux; unused here              [8B] */
	unsigned long     canary;   /* 40 : stack-protector canary                        [8B] */
	/* Part 2 — implementation detail. errno lives at a FIXED field (== musl errno_val). */
	int               tid;      /* 48 : kernel tid (set by gettid / clone)            [4B] */
	int               __errno;  /* 52 : the per-thread errno cell                     [4B] */
	/* Reserve room for musl's remaining pthread fields. */
	char              __reserved[256];
};

/* The running thread's TCB: x86_64 reads it from the thread pointer at %fs:0. Valid only
 * after arch_prctl(ARCH_SET_FS, &tcb) has installed the FS base (see crt0 / pthread
 * creation); callers in the startup path must run AFTER that. */
static inline struct __pthread *__pthread_self(void) {
	struct __pthread *self;
	__asm__("mov %%fs:0,%0" : "=r"(self));
	return self;
}

#else /* i386 */

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

#endif /* __x86_64__ */

#endif /* NX_TCB_H */
