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
 * __nx_init_tls() (exported by libc.ndl) before main(): it points the thread pointer at
 * a static TCB whose `self` field is itself, so the thread pointer resolves and errno works
 * from the very first libc call. Threads created later (Phase 4) get their own TCB + a
 * matching thread-pointer install.
 *
 *   - i386   : the thread pointer is %gs, installed via set_thread_area(2) (GDT entry 6).
 *   - x86_64 : the thread pointer is %fs.base, installed via arch_prctl(ARCH_SET_FS, &tcb)
 *              (the kernel writes IA32_FS_BASE and records it for context-switch reload).
 */
#include <nx-tcb.h>

#if defined(__x86_64__)

#define SYS_arch_prctl 158
#define ARCH_SET_FS    0x1002

/* arch_prctl(ARCH_SET_FS, addr): the x86_64 `syscall` instruction — nr in %rax, code in %rdi,
 * addr in %rsi; %rcx/%r11 are clobbered by the instruction itself (return RIP / rflags). */
static long sys_arch_prctl(int code, unsigned long addr) {
	long ret;
	__asm__ __volatile__("syscall"
			: "=a"(ret)
			: "a"((long) SYS_arch_prctl), "D"((long) code), "S"(addr)
			: "rcx", "r11", "memory");
	return ret;
}

#else /* i386 */

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

#endif /* __x86_64__ */

/* The main thread's TCB. Lives in libc.ndl's .bss, which the loader maps per-process, so
 * each process gets its own. Threads spawned via pthread_create allocate their own TCBs. */
static struct __pthread __nx_main_tcb;

/* picolibc + app code resolve errno to (*__errno_location()). Per-thread by construction. */
int *__errno_location(void) {
	return &__pthread_self()->__errno;
}

/* libc.ndl's OWN constructor array, bracketed by the hidden symbols from user/dll.ld's
 * .nx_init_array. picolibc registers startup constructors here — notably each std stream's
 * lock-init `posix_init`, which calls __retarget_lock_init to allocate the FILE's _LOCK_T.
 * NanOS links libc with -nostdlib, so no crt runs these; the loader relocates these absolute
 * pointers to the load address (--emit-relocs), but nothing CALLS them. We do, once per
 * process from __nx_init_tls() below, so stdin/stdout/stderr get real locks and stdio is
 * thread-safe (without this their _LOCK_T stays NULL and the retarget hooks no-op → unlocked
 * stdio across threads). Hidden + may be an empty range (start==end) for modules with none. */
extern void (*__nx_ctors_start[])(void) __attribute__((visibility("hidden")));
extern void (*__nx_ctors_end[])(void)   __attribute__((visibility("hidden")));

static void __nx_run_ctors(void) {
	void (**p)(void);
	for (p = __nx_ctors_start; p < __nx_ctors_end; p++)
		(*p)();
}

/* Real per-program TLS (nx_tls.c). Builds a variant-II TLS block from the template crt0 passes
 * (image/filesz/memsz/align, from the .nxe's user-nx.ld symbols), installs the thread pointer, and
 * returns the TCB — or 0 when the program has no __thread data (the common case). */
void *__nx_init_main_tls(void *image, unsigned long filesz, unsigned long memsz, unsigned long align);

/* Install the bare self-pointing TCB + thread pointer (errno + %fs:0 only). On x86_64 that is
 * %fs.base via arch_prctl(ARCH_SET_FS); on i386 the fixed TLS GDT slot (entry 6 / selector 0x33)
 * via set_thread_area. The kernel records the base and reloads the thread pointer on its way back
 * to ring 3, so %fs:0 reads `self` immediately. Then runs libc.ndl's constructors (std-stream
 * lock-init) — after the thread pointer is live (ctors may touch errno/malloc) but before crt0
 * sets `environ` (so no ctor may call getenv). */
static void __nx_init_tls_bare(void) {
	__nx_main_tcb.self = &__nx_main_tcb;

#if defined(__x86_64__)
	if (sys_arch_prctl(ARCH_SET_FS, (unsigned long) &__nx_main_tcb) != 0)
		/* TLS setup failed -> %fs:0 is invalid and the first errno access would fault or
		 * corrupt memory. Trap loudly instead of limping on with a broken thread pointer. */
		__asm__ __volatile__("int3");
#else
	struct user_desc ud;
	ud.entry_number = (unsigned int) -1;   /* "pick a slot"; kernel returns the fixed one */
	ud.base_addr    = (unsigned int) &__nx_main_tcb;
	ud.limit        = 0xFFFFF;
	ud.flags        = 0x51;                /* seg_32bit | limit_in_pages | useable */
	if (sys_set_thread_area(&ud) != 0)
		__asm__ __volatile__("int3");
#endif

	__nx_run_ctors();
}

/* Legacy 0-arg entry: the ABI the PRE-EXISTING external ports (toybox/grep/vim/bash/... whose .nxe
 * already bakes in an older crt0) call. Bare TCB only — those programs use no __thread data. Kept
 * signature-stable so their crt0 keeps working without a rebuild. crt0 calls this before any libc
 * work so errno is valid for the whole program. */
void __nx_init_tls(void) { __nx_init_tls_bare(); }

/* New entry: the current crt0 (user/crt064.S) passes the program's TLS template (per-program,
 * defined by user-nx.ld — libc.ndl can't reference those symbols). If the program has __thread
 * data (memsz>0), build a real TLS block so those variables resolve; otherwise fall back to the
 * bare TCB. Separate from __nx_init_tls to preserve the legacy 0-arg ABI above. */
void __nx_init_tls_tpl(void *tls_image, unsigned long tls_filesz,
                       unsigned long tls_memsz, unsigned long tls_align) {
	if (tls_memsz && __nx_init_main_tls(tls_image, tls_filesz, tls_memsz, tls_align)) {
		__nx_run_ctors();   /* real program TLS installed (thread pointer + errno live) */
		return;
	}
	__nx_init_tls_bare();
}
