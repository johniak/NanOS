/*
 * retarget_lock.c — picolibc retargetable locks over the kernel futex, compiled INTO libc.ndl.
 *
 * picolibc is built with _RETARGETABLE_LOCKING, so its internals (malloc, tinystdio's per-FILE
 * lock, atexit, the env table) reach for a lock through the eight __retarget_lock_* hooks plus a
 * static "big libc lock" __lock___libc_recursive_mutex. The default picolibc implementation makes
 * these no-ops (fine for a single-threaded program). Once pthreads exist, several threads can call
 * malloc()/printf() at once, so those hooks must REALLY serialize — otherwise the heap and stdio
 * buffers corrupt. We implement them here, and because this object is linked BEFORE -lc (see the
 * libc.elf rule), our definitions win and picolibc's no-op libc_misc_lock.c.o is never pulled.
 *
 * The lock is a 3-state futex mutex (Drepper, "Futexes Are Tricky"): an UNCONTENDED acquire/release
 * is a single atomic CAS/sub with NO syscall, so single-threaded programs pay almost nothing — the
 * kernel is touched only when a thread actually has to wait. struct __lock completes picolibc's
 * incomplete type; FILE locks are heap-allocated on demand at fopen, the static libc mutex is a
 * zero-initialized instance (a zeroed struct is a valid unlocked recursive mutex).
 */
#include <sys/lock.h>
#include <stdlib.h>

/* Kernel futex ABI (kernel/SyscallNr.h + SyscallDispatch.cpp): SYS_futex takes the futex word in
 * ebx, the op in ecx, the compare/wake-count value in edx, and a timeout pointer in esi (0 here).
 * The kernel requires FUTEX_PRIVATE_FLAG OR-ed into the op or it returns -ENOSYS. */
/* Syscall numbers + trap are arch-specific: i386 uses int 0x80 with the i386 numbers; x86_64
 * uses the SYSCALL instruction with the x86_64 numbers (futex=202, gettid=186) and the SysV arg
 * registers (rdi/rsi/rdx/r10; SYSCALL clobbers rcx/r11). The kernel reads the same arg slots on
 * both ABIs (futex: word, op, val, timeout). */
#if defined(__x86_64__)
#define SYS_futex            202
#define SYS_gettid           186
#else
#define SYS_futex            240
#define SYS_gettid           224
#endif
#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_PRIVATE_FLAG   128

/* WAIT blocks while *uaddr == val (returns 0 on wake, -EAGAIN if it already changed); timeout 0 =
 * block forever. We ignore the return: any return means "re-check the word", which the loops do. */
static void futex_wait(int* uaddr, int val) {
	int r;
#if defined(__x86_64__)
	register long r10 __asm__("r10") = 0;   /* timeout */
	__asm__ __volatile__("syscall" : "=a"(r)
		: "a"((long) SYS_futex), "D"((long) uaddr), "S"((long) (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)),
		  "d"((long) val), "r"(r10)
		: "rcx", "r11", "memory");
#else
	__asm__ __volatile__("int $0x80"
		: "=a"(r)
		: "a"(SYS_futex), "b"(uaddr), "c"(FUTEX_WAIT | FUTEX_PRIVATE_FLAG), "d"(val), "S"(0)
		: "memory");
#endif
	(void) r;
}

/* WAKE wakes up to `count` waiters on *uaddr. */
static void futex_wake(int* uaddr, int count) {
	int r;
#if defined(__x86_64__)
	register long r10 __asm__("r10") = 0;   /* timeout (unused for WAKE) */
	__asm__ __volatile__("syscall" : "=a"(r)
		: "a"((long) SYS_futex), "D"((long) uaddr), "S"((long) (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)),
		  "d"((long) count), "r"(r10)
		: "rcx", "r11", "memory");
#else
	__asm__ __volatile__("int $0x80"
		: "=a"(r)
		: "a"(SYS_futex), "b"(uaddr), "c"(FUTEX_WAKE | FUTEX_PRIVATE_FLAG), "d"(count), "S"(0)
		: "memory");
#endif
	(void) r;
}

static int sys_gettid(void) {
	int r;
#if defined(__x86_64__)
	long rr;
	__asm__ __volatile__("syscall" : "=a"(rr) : "a"((long) SYS_gettid) : "rcx", "r11", "memory");
	r = (int) rr;
#else
	__asm__ __volatile__("int $0x80" : "=a"(r) : "a"(SYS_gettid) : "memory");
#endif
	return r;
}

/* struct __lock completes picolibc's incomplete type. futex: 0 = unlocked, 1 = locked (no waiters),
 * 2 = locked (maybe waiters). owner/recur are used only by the recursive variants. */
struct __lock { int futex; int owner; int recur; };

/* A NULL _LOCK_T means "not yet initialized": picolibc's static std streams (stdin/stdout/stderr,
 * FDEV_SETUP_BUFIO) leave their .lock field zeroed and only __bufio_lock_init() (malloc) fills it
 * in — which never runs for them on NanOS (no global constructors). picolibc's own default lock
 * stubs ignore their argument, so a NULL lock was always a no-op; we preserve that (an uninitialized
 * lock is by definition uncontended). Real locks — fopen'd FILE locks and __lock___libc_recursive_mutex
 * — are non-NULL and lock for real. (Proper std-stream FILE locking is wired in Task 3.3.) */

/* Plain (non-recursive) mutex: 3-state futex, owner/recur untouched. */
static void lock_acquire(struct __lock* l) {
	if (!l) return;                  /* uninitialized lock -> no-op (see note above) */
	int expect = 0;
	int c = __atomic_compare_exchange_n(&l->futex, &expect, 1, 0,
		__ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 0 : expect;
	if (c != 0) {
		if (c != 2)
			c = __atomic_exchange_n(&l->futex, 2, __ATOMIC_ACQUIRE);
		while (c != 0) {
			futex_wait(&l->futex, 2);
			c = __atomic_exchange_n(&l->futex, 2, __ATOMIC_ACQUIRE);
		}
	}
}

static void lock_release(struct __lock* l) {
	if (!l) return;                  /* uninitialized lock -> no-op (see note above) */
	if (__atomic_fetch_sub(&l->futex, 1, __ATOMIC_RELEASE) != 1) {
		__atomic_store_n(&l->futex, 0, __ATOMIC_RELEASE);
		futex_wake(&l->futex, 1);
	}
}

void __retarget_lock_init(_LOCK_T* lock) {
	struct __lock* l = (struct __lock*) malloc(sizeof *l);
	if (l) { l->futex = 0; l->owner = 0; l->recur = 0; }
	*lock = l;   /* OOM: *lock stays NULL; acquire/release no-op on NULL, so the FILE just runs unlocked */
}

void __retarget_lock_init_recursive(_LOCK_T* lock) {
	__retarget_lock_init(lock);
}

void __retarget_lock_close(_LOCK_T lock) {
	free(lock);
}

void __retarget_lock_close_recursive(_LOCK_T lock) {
	free(lock);
}

void __retarget_lock_acquire(_LOCK_T lock) {
	lock_acquire(lock);
}

void __retarget_lock_release(_LOCK_T lock) {
	lock_release(lock);
}

/* Recursive mutex: the owning thread may re-acquire; only the outermost release unlocks the futex.
 * The owner read/writes use relaxed atomics to document intent (no data race in the C sense): owner
 * is only ever set to a nonzero tid by the thread holding the lock, and tids are unique, so a match
 * means THIS thread owns it. recur is touched only under the lock, so it needs no atomicity. */
void __retarget_lock_acquire_recursive(_LOCK_T lock) {
	if (!lock) return;               /* uninitialized lock -> no-op (see note above) */
	int self = sys_gettid();
	if (__atomic_load_n(&lock->owner, __ATOMIC_RELAXED) == self) {
		lock->recur++;
		return;
	}
	lock_acquire(lock);
	__atomic_store_n(&lock->owner, self, __ATOMIC_RELAXED);
	lock->recur = 1;
}

void __retarget_lock_release_recursive(_LOCK_T lock) {
	if (!lock) return;               /* uninitialized lock -> no-op (see note above) */
	if (--lock->recur == 0) {
		__atomic_store_n(&lock->owner, 0, __ATOMIC_RELAXED);   /* clear owner BEFORE releasing the futex */
		lock_release(lock);
	}
}

/* picolibc's "big libc lock" instance (declared by __LOCK_INIT_RECURSIVE in <sys/lock.h>). A zeroed
 * struct is a valid UNLOCKED recursive mutex, so no runtime init is needed. */
struct __lock __lock___libc_recursive_mutex = { 0, 0, 0 };
