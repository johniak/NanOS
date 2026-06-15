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
#define SYS_futex            240
#define SYS_gettid           224
#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_PRIVATE_FLAG   128

/* WAIT blocks while *uaddr == val (returns 0 on wake, -EAGAIN if it already changed); timeout 0 =
 * block forever. We ignore the return: any return means "re-check the word", which the loops do. */
static void futex_wait(int* uaddr, int val) {
	int r;
	__asm__ __volatile__("int $0x80"
		: "=a"(r)
		: "a"(SYS_futex), "b"(uaddr), "c"(FUTEX_WAIT | FUTEX_PRIVATE_FLAG), "d"(val), "S"(0)
		: "memory");
	(void) r;
}

/* WAKE wakes up to `count` waiters on *uaddr. */
static void futex_wake(int* uaddr, int count) {
	int r;
	__asm__ __volatile__("int $0x80"
		: "=a"(r)
		: "a"(SYS_futex), "b"(uaddr), "c"(FUTEX_WAKE | FUTEX_PRIVATE_FLAG), "d"(count), "S"(0)
		: "memory");
	(void) r;
}

static int sys_gettid(void) {
	int r;
	__asm__ __volatile__("int $0x80" : "=a"(r) : "a"(SYS_gettid) : "memory");
	return r;
}

/* struct __lock completes picolibc's incomplete type. futex: 0 = unlocked, 1 = locked (no waiters),
 * 2 = locked (maybe waiters). owner/recur are used only by the recursive variants. */
struct __lock { int futex; int owner; int recur; };

/* Plain (non-recursive) mutex: 3-state futex, owner/recur untouched. */
static void lock_acquire(struct __lock* l) {
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
	if (__atomic_fetch_sub(&l->futex, 1, __ATOMIC_RELEASE) != 1) {
		__atomic_store_n(&l->futex, 0, __ATOMIC_RELEASE);
		futex_wake(&l->futex, 1);
	}
}

void __retarget_lock_init(_LOCK_T* lock) {
	struct __lock* l = (struct __lock*) malloc(sizeof *l);
	if (l) { l->futex = 0; l->owner = 0; l->recur = 0; }
	*lock = l;
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

/* Recursive mutex: the owning thread may re-acquire; only the outermost release unlocks the futex. */
void __retarget_lock_acquire_recursive(_LOCK_T lock) {
	int self = sys_gettid();
	if (lock->owner == self) {
		lock->recur++;
		return;
	}
	lock_acquire(lock);
	lock->owner = self;
	lock->recur = 1;
}

void __retarget_lock_release_recursive(_LOCK_T lock) {
	if (--lock->recur == 0) {
		lock->owner = 0;
		lock_release(lock);
	}
}

/* picolibc's "big libc lock" instance (declared by __LOCK_INIT_RECURSIVE in <sys/lock.h>). A zeroed
 * struct is a valid UNLOCKED recursive mutex, so no runtime init is needed. */
struct __lock __lock___libc_recursive_mutex = { 0, 0, 0 };
