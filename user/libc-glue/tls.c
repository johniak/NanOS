/*
 * tls.c — per-thread errno + main-thread TLS bootstrap, compiled INTO libc.ndl.
 *
 * picolibc is built with `-Derrno-function=__errno_location`, so both application code
 * and picolibc's own internals reach errno through `(*__errno_location())`. We provide
 * that function here: it returns the address of the CURRENT thread's errno cell, read
 * from the TCB at the thread pointer (%gs:0). One cell per thread ⇒ a thread-safe errno
 * with no shared global (the old `__imp_errno` data slot is gone).
 *
 * For the MAIN thread there is no pthread_create to set up TLS, so crt0 calls
 * __nx_init_tls() (exported by libc.ndl) before main(): it points the TLS descriptor at
 * a static TCB whose `self` field is itself, so %gs:0 resolves and errno works from the
 * very first libc call. Threads created later (Phase 4) get their own TCB + set_thread_area.
 */
#include <nx-tcb.h>

#define SYS_set_thread_area 243

/* Linux i386 struct user_desc, as set_thread_area(2) consumes it (see kernel/ThreadArea.h). */
struct user_desc {
	unsigned int entry_number;
	unsigned int base_addr;
	unsigned int limit;
	unsigned int flags;
};

static int sys_set_thread_area(struct user_desc *ud) {
	int ret;
	__asm__ __volatile__("int $0x80"
			: "=a"(ret)
			: "a"(SYS_set_thread_area), "b"(ud)
			: "memory");
	return ret;
}

/* The main thread's TCB. Lives in libc.ndl's .bss, which the loader maps per-process, so
 * each process gets its own. Threads spawned via pthread_create allocate their own TCBs. */
static struct __pthread __nx_main_tcb;

/* picolibc + app code resolve errno to (*__errno_location()). Per-thread by construction. */
int *__errno_location(void) {
	return &__pthread_self()->__errno;
}

/* Install the main thread's TLS: self-point the TCB and aim the fixed TLS GDT slot
 * (entry 6 / selector 0x33) at it via set_thread_area. The kernel reloads %gs on the way
 * back to ring 3, so %gs:0 reads `self` immediately after this returns. Called from crt0
 * (_start) before any other libc work, so errno is valid for the whole program. */
void __nx_init_tls(void) {
	__nx_main_tcb.self = &__nx_main_tcb;

	struct user_desc ud;
	ud.entry_number = (unsigned int) -1;   /* "pick a slot"; kernel returns the fixed one */
	ud.base_addr    = (unsigned int) &__nx_main_tcb;
	ud.limit        = 0xFFFFF;
	ud.flags        = 0x51;                /* seg_32bit | limit_in_pages | useable */
	if (sys_set_thread_area(&ud) != 0)
		/* TLS setup failed -> %gs:0 is invalid and the first errno access would fault or
		 * corrupt memory. Trap loudly instead of limping on with a broken thread pointer. */
		__asm__ __volatile__("int3");
}
