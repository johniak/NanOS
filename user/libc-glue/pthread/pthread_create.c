/*
 * pthread_create.c — NanOS adaptation of musl 1.2.5 src/thread/pthread_create.c.
 *
 * WHY ADAPTED (not verbatim): upstream musl's pthread_create is entangled with machinery
 * NanOS does not have yet — per-thread signal masking (__block_app_sigs / __restore_sigs +
 * the rt_sigprocmask in the child's start()), the vm-lock (__vm_wait), the global
 * thread-list lock (__tl_lock), the robust-mutex list, and __copy_tls building a real TLS
 * image from .tdata modules. None of that is exercised by the non-cancel, non-signal happy
 * path this milestone targets. We therefore KEEP musl's TCB semantics (struct pthread,
 * detach_state enum, map_base/map_size/stack/result), the struct start_args hand-off and the
 * i386 __clone ABI verbatim, but DROP the signal / vmlock / tl-lock / robust / TLS-image code.
 * Per-thread signals and cancellation land in Phase 5.
 *
 * JOIN HANDSHAKE: the NanOS kernel honours CLONE_CHILD_CLEARTID — on thread exit it zeroes the
 * ctid word and futex-wakes it (procThreadExit, Task 2.3). We point ctid at &tcb->tid, so
 * pthread_join just FUTEX_WAITs on &tcb->tid until the kernel clears it, exactly as the raw
 * clonetest did. __pthread_exit only records the result and SYS_exit(0)s; the kernel performs
 * the wake. (Upstream musl instead wakes &detach_state from userspace; leaning on the kernel
 * here is simpler and already proven on NanOS.)
 *
 * STACK / TCB LIFETIME: NanOS munmap is a no-op — a process's pages are reclaimed only at
 * process exit (see user/libc-glue/syscalls.c). So a thread's stack+TCB are never actually
 * freed: pthread_join can safely read tcb->result after the thread has exited, and a detached
 * thread simply leaks its stack until the process ends. Documented limitation for Phase 4.
 */
#include "pthread_impl.h"
#include <sys/mman.h>
#include <string.h>

/* clone(2) flags — Linux i386 ABI (see kernel/CloneFlags.h). Defined locally rather than via
 * <sched.h> (picolibc's lacks the CLONE_* set), matching the clonetest precedent. */
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

/* A single shared, never-dereferenced empty dtv/tsd. NanOS userland has no __thread TLS image
 * (no .tdata modules in the .nxe loader), so __tls_get_addr is never called; the slot only has
 * to be a valid, non-NULL pointer so musl's TCB invariants hold. */
static uintptr_t dummy_dtv[1];
static void     *dummy_tsd[1];

/* The argument block handed to the child's entry trampoline, placed in the new thread's own
 * mapping (shared via CLONE_VM) just below its TCB. */
struct start_args {
	void *(*start_func)(void *);
	void *start_arg;
	struct pthread *self;
};

_Noreturn void __pthread_exit(void *result);   /* defined below; used by start() above it */

/* The child's first C code. %gs (and thus %gs:0 -> self and errno) was already installed by
 * the kernel from the CLONE_SETTLS user_desc that __clone built, so the libc is usable from
 * the first instruction. No per-thread signal unmask here (Phase 5). Returning is impossible:
 * __pthread_exit never comes back; the trailing return only satisfies __clone's int(*)(void*). */
static int start(void *p)
{
	struct start_args *args = p;
	__pthread_exit(args->start_func(args->start_arg));
	return 0;
}

#define ALIGN_DOWN(p, a) ((void *)((uintptr_t)(p) & ~((uintptr_t)(a) - 1)))

_Noreturn void __pthread_exit(void *result)
{
	pthread_t self = __pthread_self();
	self->result = result;
	self->canceldisable = 1;
	/* Mark exited for introspection/detach symmetry; the join wakeup itself rides the
	 * kernel's CLONE_CHILD_CLEARTID on &self->tid, not this store. */
	if (self->detach_state != DT_DETACHED)
		a_store(&self->detach_state, DT_EXITED);
	/* Per-thread exit: kernel zeroes the ctid (&self->tid) + futex-wakes the joiner. result
	 * stays readable afterwards because NanOS never unmaps the TCB. */
	for (;;) __syscall(SYS_exit, 0);
}
weak_alias(__pthread_exit, pthread_exit);

int __pthread_create(pthread_t *restrict res, const pthread_attr_t *restrict attrp,
                     void *(*entry)(void *), void *restrict arg)
{
	if (!entry) return EINVAL;

	pthread_attr_t attr = { 0 };
	if (attrp && attrp != __ATTRP_C11_THREAD)
		attr = *attrp;

	size_t guard   = attr._a_guardsize ? attr._a_guardsize : DEFAULT_GUARD_SIZE;
	size_t stacksz = attr._a_stacksize ? attr._a_stacksize : DEFAULT_STACK_SIZE;
	guard   = (guard   + 4095) & ~(size_t)4095;
	stacksz = (stacksz + 4095) & ~(size_t)4095;

	/* One mapping holds, high to low: [TCB][start_args][stack ... grows down][guard].
	 * NanOS has no page-permission guard (mprotect is best-effort), so the guard is just
	 * reserved address space below the stack. */
	size_t total = guard + stacksz + sizeof(struct start_args) + sizeof(struct pthread) + 64;

	unsigned char *map = mmap(0, total, PROT_READ | PROT_WRITE,
	                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map == MAP_FAILED)
		return EAGAIN;

	struct pthread    *new    = ALIGN_DOWN(map + total - sizeof(struct pthread), 16);
	struct start_args *stargs = ALIGN_DOWN((unsigned char *)new - sizeof(struct start_args), 16);
	unsigned char     *stack_top = ALIGN_DOWN(stargs, 16);

	memset(new, 0, sizeof *new);
	new->self         = new;                 /* %gs:0 -> self */
	new->map_base     = map;
	new->map_size     = total;
	new->stack        = stack_top;
	new->stack_size   = (size_t)(stack_top - (map + guard));
	new->guard_size   = guard;
	new->dtv          = dummy_dtv;
	new->tsd          = dummy_tsd;
	new->detach_state = (attr._a_detach == PTHREAD_CREATE_DETACHED) ? DT_DETACHED : DT_JOINABLE;
	new->tid          = -1;                  /* kernel overwrites via PARENT/CHILD_SETTID */

	stargs->start_func = entry;
	stargs->start_arg  = arg;
	stargs->self       = new;

	int flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD
	          | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID;

	/* We are now multi-threaded: turn on libc's internal locking (malloc/stdio/__lock all
	 * gate on need_locks) before the second thread can run. INTENTIONALLY one-way: we never
	 * reset threaded/need_locks nor decrement threads_minus_1 on thread exit (only on the
	 * create-failure rollback below). Leaving locking on after the last thread joins is
	 * conservatively safe (a redundant uncontended lock), and avoids a class of races around
	 * turning locking back off; the counter only grows, which is harmless in practice. */
	libc.threaded = 1;
	libc.need_locks = 1;
	a_inc(&libc.threads_minus_1);

	/* __clone(fn, stack_top, flags, arg, ptid, tls, ctid). tls == the TCB base; __clone's asm
	 * wraps it in the set_thread_area user_desc the kernel reads for CLONE_SETTLS. ptid==ctid==
	 * &new->tid so the kernel seeds the tid AND, on exit, clears+wakes that same word (join). */
	int ret = __clone(start, stack_top, flags, stargs, &new->tid, TP_ADJ(new), &new->tid);
	if (ret < 0) {
		a_dec(&libc.threads_minus_1);
		return EAGAIN;
	}

	*res = new;
	return 0;
}
weak_alias(__pthread_create, pthread_create);
