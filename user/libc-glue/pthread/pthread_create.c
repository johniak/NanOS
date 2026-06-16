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
 * STACK / TCB LIFETIME: munmap is now real — it clears the PTEs, frees the backing frames,
 * AND records the VA for reuse (kernel SYS_munmap + the mmap free-list; see syscalls.c +
 * kernel/SyscallDispatch.cpp). pthread_join therefore munmaps the joined thread's
 * map_base/map_size, so a long-running create/join churn recycles the finite (64 MiB) mmap
 * window instead of exhausting it. (The join handshake below still rides the kernel ctid
 * wake, so the joiner only munmaps AFTER the thread has exited — its stack/TCB stay live
 * until then.) LIMITATION: a DETACHED thread cannot unmap its own running stack and nobody
 * joins it, so its stack+TCB still leak until process exit (the portable fix is musl's
 * __unmapself i386 trampoline — not vendored here; see docs/en/threads.md).
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

/* A single shared, never-dereferenced empty dtv. NanOS userland has no __thread TLS image
 * (no .tdata modules in the .nxe loader), so __tls_get_addr is never called; the slot only has
 * to be a valid, non-NULL pointer so musl's TCB invariants hold. (tsd, by contrast, must be a
 * real PER-THREAD array — pthread_get/setspecific index self->tsd[key] for key<PTHREAD_KEYS_MAX
 * — so each thread gets its own zeroed tsd[] carved out of its mapping below, not a shared one.) */
static uintptr_t dummy_dtv[1];

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

/* Kernel SIGCANCEL (kernel/Signal.h). NOT musl's pthread_impl.h SIGCANCEL (33). */
#define NX_SIGCANCEL 32

/* Kernel-ABI sigaction record (mirror of struct k_sigaction in kernel/SyscallNr.h). */
struct nx_k_sigaction {
	void          *handler;
	unsigned long  flags;
	void          *restorer;
	unsigned       mask[2];
};
extern void __nx_sigtramp(void);   /* libc sigreturn trampoline (user/libc-glue) */

/* The SIGCANCEL handler is intentionally EMPTY. Its only job is to give SIGCANCEL a "run a
 * handler" disposition so that delivering it INTERRUPTS a blocked cancellable futex (returning
 * -EINTR to __syscall_cp) instead of taking the RT-signal default action (terminate the
 * thread). The actual cancellation work happens back in __syscall_cp/__cancel. */
static void __nx_cancel_sigcancel(int sig) { (void) sig; }

/* Install the SIGCANCEL no-op handler exactly once, before the first additional thread can run.
 * Done with a raw rt_sigaction so it does not depend on picolibc's <signal.h> wrappers and so we
 * can target the kernel's SIGCANCEL (32) directly. flags=0: the kernel forces SIGCANCEL
 * non-restarting regardless (kernel/Exec.cpp signalActionRt), so an interrupted cancellable
 * futex returns -EINTR rather than restarting. The restorer is the libc sigreturn trampoline. */
static void __nx_install_sigcancel(void)
{
	struct nx_k_sigaction act = { 0 };
	act.handler  = (void *) &__nx_cancel_sigcancel;
	act.flags    = 0;
	act.restorer = (void *) &__nx_sigtramp;
	__syscall(SYS_rt_sigaction, NX_SIGCANCEL, &act, 0, 8);
}

/* Cleanup-handler chain (pthread_cleanup_push/pop). These REAL definitions override the weak
 * `dummy` no-ops in pthread_cleanup_push.c, linking each __ptcb onto self->cancelbuf so that
 * __pthread_exit can run them (newest first) on the way out — required by POSIX both for normal
 * thread return and for cancellation (which exits via __cancel -> __pthread_exit). */
void __do_cleanup_push(struct __ptcb *cb)
{
	pthread_t self = __pthread_self();
	cb->__next = self->cancelbuf;
	self->cancelbuf = cb;
}

void __do_cleanup_pop(struct __ptcb *cb)
{
	__pthread_self()->cancelbuf = cb->__next;
}

_Noreturn void __pthread_exit(void *result)
{
	pthread_t self = __pthread_self();
	self->result = result;
	/* No more cancellation from here on — we are already leaving. */
	self->canceldisable = 1;
	self->cancelasync = 0;

	/* Run pthread_cleanup_push'd handlers, newest first (POSIX). This is the cancellation-
	 * critical part: a thread cancelled in a cond/sem wait reaches here via __cancel and its
	 * cleanup handlers (e.g. mutex unlock, resource release) MUST run. Normal thread return
	 * comes through the same path, so its handlers run too. (TSD destructor sweep stays
	 * deferred — not needed by the current threaded apps.) */
	while (self->cancelbuf) {
		void (*f)(void *) = self->cancelbuf->__f;
		void *x = self->cancelbuf->__x;
		self->cancelbuf = self->cancelbuf->__next;
		f(x);
	}

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

	/* One mapping holds, high to low: [TCB][start_args][tsd[]][stack ... grows down][guard].
	 * NanOS has no page-permission guard (mprotect is best-effort), so the guard is just
	 * reserved address space below the stack. The tsd[] block is this thread's private
	 * pthread_setspecific storage (PTHREAD_KEYS_MAX void*), zero by virtue of MAP_ANONYMOUS. */
	size_t tsd_size = sizeof(void *) * PTHREAD_KEYS_MAX;
	size_t total = guard + stacksz + tsd_size + sizeof(struct start_args)
	             + sizeof(struct pthread) + 64;

	unsigned char *map = mmap(0, total, PROT_READ | PROT_WRITE,
	                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map == MAP_FAILED)
		return EAGAIN;

	struct pthread    *new    = ALIGN_DOWN(map + total - sizeof(struct pthread), 16);
	struct start_args *stargs = ALIGN_DOWN((unsigned char *)new - sizeof(struct start_args), 16);
	void             **tsd    = (void **)ALIGN_DOWN((unsigned char *)stargs - tsd_size, 16);
	unsigned char     *stack_top = ALIGN_DOWN((unsigned char *)tsd, 16);

	memset(new, 0, sizeof *new);
	new->self         = new;                 /* %gs:0 -> self */
	new->map_base     = map;
	new->map_size     = total;
	new->stack        = stack_top;
	new->stack_size   = (size_t)(stack_top - (map + guard));
	new->guard_size   = guard;
	new->dtv          = dummy_dtv;
	new->tsd          = tsd;                 /* per-thread pthread_setspecific storage (zeroed) */
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
	if (!libc.threaded)
		__nx_install_sigcancel();   /* SIGCANCEL disposition must exist before any thread runs */
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
