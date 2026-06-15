/*
 * clonetest — the Phase 0-2 pthread keystone smoke test, in RAW syscalls (no libc threads).
 *
 * It proves the kernel thread machinery end to end:
 *   - set_thread_area(2)  installs a TLS descriptor (Task 2.2),
 *   - clone(2)            spawns a second thread sharing this address space (Task 2.3),
 *   - the child writes a shared global and exit(2)s, which (with two live threads) runs the
 *     per-thread exit: CLONE_CHILD_CLEARTID zeroes the tid word + futex-wakes the joiner (2.3),
 *   - the parent futex(FUTEX_WAIT|FUTEX_PRIVATE_FLAG, ...) loops on the tid word until the
 *     child clears it (the pthread_join handshake, Tasks 1.x), then reports the result.
 *
 * No printf / malloc dependency: output goes through raw write(2). The clone itself uses the
 * naked asm helper in clone_helper.S (the safe musl-style idiom).
 */

/* --- syscall numbers (this kernel; see kernel/SyscallNr.h) --- */
#define SYS_exit            1
#define SYS_write           4
#define SYS_futex           240
#define SYS_set_thread_area 243

/* --- clone(2) flags (Linux i386 ABI; see kernel/CloneFlags.h) --- */
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000   /* kernel sets *ctid = tid at clone time (race-free join) */

/* --- futex(2) --- */
#define FUTEX_WAIT          0
#define FUTEX_PRIVATE_FLAG  128

/* Linux i386 struct user_desc, as set_thread_area(2) consumes it (see kernel/ThreadArea.h). */
struct user_desc {
	unsigned int entry_number;
	unsigned int base_addr;
	unsigned int limit;
	unsigned int flags;
};

/* int 0x80 wrapper carrying up to four args (ebx/ecx/edx/esi). Enough for futex's
 * (uaddr, op, val, timeout) and set_thread_area's single pointer arg. */
static int syscall4(int nr, int a0, int a1, int a2, int a3) {
	int ret;
	__asm__ __volatile__("int $0x80"
			: "=a"(ret)
			: "a"(nr), "b"(a0), "c"(a1), "d"(a2), "S"(a3)
			: "memory");
	return ret;
}

/* The asm clone launcher (user/clone_helper.S). Returns the new tid (>0) to the parent. */
extern int clone_thread(unsigned flags, void* child_stack_top, int* ctid, void (*fn)(void));

static int sys_write(int fd, const void* buf, unsigned n) {
	return syscall4(SYS_write, fd, (int) buf, (int) n, 0);
}
static int sys_futex_wait(int* uaddr, int val) {
	return syscall4(SYS_futex, (int) uaddr, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, val, 0 /* no timeout */);
}
static int sys_set_thread_area(struct user_desc* ud) {
	return syscall4(SYS_set_thread_area, (int) ud, 0, 0, 0);
}

static unsigned slen(const char* s) {
	unsigned n = 0;
	while (s[n]) n++;
	return n;
}
static void puts1(const char* s) { sys_write(1, s, slen(s)); }

/* --- the shared state the child mutates and the parent verifies --- */
static volatile int shared = 0;     /* set to 42 by the child */
static int tidword = 0;             /* the CLEARTID join word (CHILD_SETTID seeds it with the tid) */

/* The child thread runs on its own stack (below) with a copy of the parent's registers. It
 * touches only the shared global, then returns to the asm helper, which exit(0)s the thread. */
static void child_fn(void) {
	shared = 42;
}

/* The child's stack. The address space is shared (CLONE_VM), so a static array is fine; clone
 * is handed the TOP of it. 16 KiB is ample for this trivial frame. */
static unsigned char child_stack[16 * 1024] __attribute__((aligned(16)));

/* A small TLS block so set_thread_area has something to point the descriptor base at. */
static unsigned char tls_block[256] __attribute__((aligned(16)));

int main(void) {
	puts1("clonetest: start\n");

	/* (1) Exercise set_thread_area (Task 2.2): point the fixed TLS slot at our block. The
	 * kernel writes entry_number back (6) and loads GDT entry 6; we do not deref %gs here. */
	struct user_desc ud;
	ud.entry_number = (unsigned) -1;          /* "pick a slot"; kernel returns the fixed one */
	ud.base_addr    = (unsigned) tls_block;
	ud.limit        = 0xFFFFF;
	ud.flags        = 0x51;                    /* seg_32bit | limit_in_pages | useable */
	int sta = sys_set_thread_area(&ud);
	puts1(sta >= 0 ? "clonetest: set_thread_area ok\n" : "clonetest: set_thread_area FAIL\n");

	/* (2) Stack top, 16-byte aligned (grows down). */
	void* stack_top = (void*) (((unsigned) child_stack + sizeof(child_stack)) & ~15u);

	/* (3) clone the child thread. CHILD_SETTID makes the kernel store the new tid into
	 * tidword at clone time, so the join loop below is race-free even if the child exits
	 * before we first look. CHILD_CLEARTID makes its exit zero tidword + futex-wake us. */
	unsigned flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD
	               | CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID;
	int tid = clone_thread(flags, stack_top, &tidword, child_fn);
	if (tid <= 0) {
		puts1("clonetest: clone FAIL\n");
		return 1;
	}
	puts1("clonetest: cloned child\n");

	/* (4) Join: block in FUTEX_WAIT until the child clears tidword. Re-reads the word each
	 * pass; FUTEX_WAIT returns immediately (-EAGAIN) if it no longer equals the expected
	 * value, so a spurious or already-cleared word just falls through the loop. */
	for (;;) {
		int v = tidword;
		if (v == 0)
			break;                 /* child has exited and cleared it */
		sys_futex_wait(&tidword, v);
	}

	/* (5) Report. */
	if (shared == 42)
		puts1("clonetest: shared=42 join ok\n");
	else
		puts1("clonetest: shared!=42 join FAIL\n");

	return shared == 42 ? 0 : 1;
}
