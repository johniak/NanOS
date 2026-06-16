#include "SyscallDispatch.h"
#include "Syscall.h"
#include "Process.h"
#include "Exec.h"
#include "SignalDispatch.h"
#include "Scheduler.h"
#include "Futex.h"
#include "ThreadArea.h"
#include "Clock.h"
#include "Csprng.h"
#include <arch/syscall.h>
#include <arch/console.h>
#include <arch/cpu.h>
#include <arch/input.h>
#include <arch/mmu.h>
#include <arch/sched.h>
#include "Console.h"

namespace kernel {

static Vfs* g_vfs = 0;   // for SYS_execve (load a .nxe from the VFS)

// Bounded copy of a user C-string into a kernel buffer (NUL-terminated).
static void copyStr(char* dst, const char* src, int cap) {
	int i = 0;
	if (src)
		for (; i < cap - 1 && src[i]; i++)
			dst[i] = src[i];
	dst[i] = 0;
}

// execve argument limits. A single shared buffer packs all argv+envp strings (mirroring
// Linux's page-bounded ARG_MAX) and the pointer arrays cap the vector length — no fixed
// per-string limit. Overflowing either is reported as -E2BIG, never a silent truncation.
static const int ARG_STRBYTES = 16384;
static const int ARG_MAXVEC   = 128;

// Copy a NULL-terminated user string vector into the packed buffer `buf` (cap ARG_STRBYTES,
// running cursor *used) and fill ptrs[] (cap ARG_MAXVEC). Returns the count, or -E2BIG if
// either the vector length or the byte budget would be exceeded.
static int copyVec(const char* const* uvec, const char** ptrs, char* buf, int* used) {
	int n = 0;
	if (!uvec) return 0;
	for (; uvec[n]; n++) {
		if (n >= ARG_MAXVEC) return -E2BIG;
		const char* s = uvec[n];
		ptrs[n] = buf + *used;
		int i = 0;
		while (s[i]) {
			if (*used + i >= ARG_STRBYTES - 1) return -E2BIG;
			buf[*used + i] = s[i];
			i++;
		}
		buf[*used + i] = 0;
		*used += i + 1;
	}
	return n;
}

static int consoleSink(const char* buf, unsigned len) {
	for (unsigned i = 0; i < len; i++)
		Console::write(buf[i]);
	return (int) len;
}

// pollScan() reports the console as always POLLIN-ready because it is MI and can't see the
// keyboard layer; refine that here with the real state (arch::inputReady) so poll()/select()
// on the console blocks until a key is actually available. readline depends on this: it
// batches its redisplay while input looks pending, so a console that always claims "readable"
// makes it defer the echo of every keystroke until Enter. Returns the corrected ready count.
static int pollScanConsoleAware(Syscalls* g_sys, PollFd* pfds, int nfds) {
	int ret = g_sys->pollScan(pfds, nfds);
	if (arch::inputReady())
		return ret;
	int n = 0;
	for (int i = 0; i < nfds; i++) {
		if (pfds[i].fd >= 0 && g_sys->isConsoleFd(pfds[i].fd))
			pfds[i].revents &= ~POLLIN;     // no key buffered -> not readable
		if (pfds[i].revents)
			n++;
	}
	return n;
}

// ---- Sockets (FAZA 9) -------------------------------------------------------------------
// One unified handler for both the socketcall(2) demux and the direct socket syscalls. A[0..5]
// are the already-extracted arguments. Blocking (connect/accept/recv/send) is handled here,
// event-driven on the socket's wait queue, exactly like the pipe read/write loops.

static bool sockBlock(Syscalls* g, int fd, int* err) {
	if (hasPendingSignalCurrent()) { *err = -ERESTARTSYS; return false; }
	WaitQueue* wq = g->fdWaitQueue(fd);
	if (wq) Scheduler::sleepOn(wq); else Scheduler::ioWait();
	if (hasPendingSignalCurrent()) { *err = -ERESTARTSYS; return false; }
	return true;
}

// Gather a user iovec[] into kdst (cap), or scatter from ksrc into it. Returns total bytes
// moved. Bounded — fine for our datagram/stream uses (DNS/HTTP); larger is truncated honestly.
static int iovGather(const unsigned* iov, unsigned iovlen, char* kdst, int cap) {
	int total = 0;
	for (unsigned i = 0; i < iovlen && total < cap; i++) {
		const char* base = (const char*) iov[i * 2];
		int len = (int) iov[i * 2 + 1];
		for (int k = 0; k < len && total < cap; k++) kdst[total++] = base[k];
	}
	return total;
}
static void iovScatter(const unsigned* iov, unsigned iovlen, const char* ksrc, int n) {
	int off = 0;
	for (unsigned i = 0; i < iovlen && off < n; i++) {
		char* base = (char*) iov[i * 2];
		int len = (int) iov[i * 2 + 1];
		for (int k = 0; k < len && off < n; k++) base[k] = ksrc[off++];
	}
}

static int socketOp(Syscalls* g, int sub, const unsigned* A) {
	int fd = (int) A[0];
	switch (sub) {
	case SC_SOCKET:
		return g->sockSocket((int) A[0], (int) A[1], (int) A[2]);
	case SC_BIND:
		return g->sockBind(fd, (const void*) A[1], A[2]);
	case SC_LISTEN:
		return g->sockListen(fd, (int) A[1]);
	case SC_SETSOCKOPT:
		return g->sockSetsockopt(fd, (int) A[1], (int) A[2], (const void*) A[3], A[4]);
	case SC_GETSOCKOPT:
		return g->sockGetsockopt(fd, (int) A[1], (int) A[2], (void*) A[3], (unsigned*) A[4]);
	case SC_GETSOCKNAME:
		return g->sockGetsockname(fd, (void*) A[1], (unsigned*) A[2]);
	case SC_GETPEERNAME:
		return g->sockGetpeername(fd, (void*) A[1], (unsigned*) A[2]);
	case SC_SHUTDOWN:
		return g->sockShutdown(fd, (int) A[1]);
	case SC_SOCKETPAIR:
		return g->sockSocketpair((int) A[0], (int) A[1], (int) A[2], (int*) A[3]);
	case SC_CONNECT: {
		int r = g->sockConnect(fd, (const void*) A[1], A[2]);
		if (r != -EINPROGRESS) return r;
		if (g->nonblock(fd)) return -EINPROGRESS;
		for (;;) {                                   // blocking connect: wait for the handshake
			int cr = g->sockConnectResult(fd);
			if (cr != -EINPROGRESS) return cr;
			int e; if (!sockBlock(g, fd, &e)) return e;
		}
	}
	case SC_ACCEPT:
	case SC_ACCEPT4: {
		for (;;) {
			int r = g->sockAccept(fd, (void*) A[1], (unsigned*) A[2]);
			if (r != -EAGAIN) return r;
			if (g->nonblock(fd)) return -EAGAIN;
			int e; if (!sockBlock(g, fd, &e)) return e;
		}
	}
	case SC_SEND:
		return g->sockSendto(fd, (const void*) A[1], A[2], (int) A[3], 0, 0);
	case SC_SENDTO:
		return g->sockSendto(fd, (const void*) A[1], A[2], (int) A[3], (const void*) A[4], A[5]);
	case SC_RECV:
	case SC_RECVFROM: {
		void* sa = (sub == SC_RECVFROM) ? (void*) A[4] : 0;
		unsigned* sl = (sub == SC_RECVFROM) ? (unsigned*) A[5] : 0;
		int flags = (int) A[3];
		for (;;) {
			int r = g->sockRecvfrom(fd, (void*) A[1], A[2], flags, sa, sl);
			if (r != -EAGAIN) return r;
			if (g->nonblock(fd) || (flags & 0x40 /*MSG_DONTWAIT*/)) return -EAGAIN;
			int e; if (!sockBlock(g, fd, &e)) return e;
		}
	}
	case SC_SENDMSG: {
		const unsigned* m = (const unsigned*) A[1];   // struct msghdr
		if (!m) return -EINVAL;
		static char kbuf[8192];   // restored from 4096 after the user-window move freed kernel BSS
		int n = iovGather((const unsigned*) m[2], m[3], kbuf, sizeof(kbuf));
		return g->sockSendto(fd, kbuf, (unsigned) n, (int) A[2], (const void*) m[0], m[1]);
	}
	case SC_RECVMSG: {
		unsigned* m = (unsigned*) A[1];
		if (!m) return -EINVAL;
		static char kbuf[8192];   // restored from 4096 after the user-window move freed kernel BSS
		int flags = (int) A[2];
		for (;;) {
			int n = g->sockRecvfrom(fd, kbuf, sizeof(kbuf), flags, (void*) m[0], (unsigned*) &m[1]);
			if (n == -EAGAIN && !g->nonblock(fd) && !(flags & 0x40)) { int e; if (!sockBlock(g, fd, &e)) return e; continue; }
			if (n < 0) return n;
			iovScatter((const unsigned*) m[2], m[3], kbuf, n);
			m[5] = 0;   // msg_controllen: we produce no ancillary data, so report 0 control bytes
			            // (Linux overwrites the caller's input length). Leaving it non-zero makes
			            // CMSG_FIRSTHDR walk the caller's uninitialised cmsg buffer — a garbage
			            // cmsg_len can spin CMSG_NXTHDR forever (e.g. busybox udhcpc's auxdata loop).
			m[6] = 0;   // msg_flags
			return n;
		}
	}
	default:
		return -38;   // -ENOSYS
	}
}

// select(2) (i386 _newselect): convert the fd_sets to a poll scan, block with the timeout.
static int doSelect(Syscalls* g, int nfds, unsigned* rfds, unsigned* wfds, unsigned* efds, unsigned* tv) {
	if (nfds < 0) return -EINVAL;
	if (nfds > 128) nfds = 128;     // our fd table is 128 wide
	auto isset = [](unsigned* s, int fd) { return s && (s[fd / 32] & (1u << (fd % 32))); };
	// Build a pollfd array from the requested sets.
	PollFd pf[128]; int np = 0;
	for (int fd = 0; fd < nfds; fd++) {
		short ev = 0;
		if (isset(rfds, fd)) ev |= POLLIN;
		if (isset(wfds, fd)) ev |= POLLOUT;
		if (isset(efds, fd)) ev |= POLLERR;
		if (ev) { pf[np].fd = fd; pf[np].events = ev; pf[np].revents = 0; np++; }
	}
	// timeout (struct timeval{sec,usec} i386) in ms; null = block forever.
	int timeoutMs = -1;
	if (tv) timeoutMs = (int) (tv[0] * 1000u + (tv[1] + 999u) / 1000u);
	unsigned start = Scheduler::ticks();
	for (;;) {
		int ready = pollScanConsoleAware(g, pf, np);
		if (ready > 0 || timeoutMs == 0) {
			// Rewrite the fd_sets to only the ready fds.
			int words = (nfds + 31) / 32;
			for (int w = 0; w < words; w++) { if (rfds) rfds[w] = 0; if (wfds) wfds[w] = 0; if (efds) efds[w] = 0; }
			int count = 0;
			for (int i = 0; i < np; i++) {
				short re = pf[i].revents; int fd = pf[i].fd;
				bool any = false;
				if (rfds && (re & POLLIN))  { rfds[fd/32] |= 1u << (fd%32); any = true; }
				if (wfds && (re & POLLOUT)) { wfds[fd/32] |= 1u << (fd%32); any = true; }
				if (efds && (re & (POLLERR|POLLHUP))) { efds[fd/32] |= 1u << (fd%32); any = true; }
				if (any) count++;
			}
			return count;
		}
		if (hasPendingSignalCurrent()) return -ERESTARTSYS;
		if (timeoutMs > 0 && Scheduler::ticks() - start >= (unsigned) timeoutMs) return 0;
		Scheduler::ioWait();
	}
}

// ---- futex(2) (Task 1.2) ----------------------------------------------------------------
// Wires the pure FutexTable onto the scheduler's block/wake primitives. Private-only: a
// futex without FUTEX_PRIVATE_FLAG is process-shared (out of scope) -> -ENOSYS. The key is
// (AddressSpace*, uaddr); the table only links caller-owned waiter nodes (here, on this
// task's kernel stack), so there is no allocation. The compare-and-enqueue runs with
// interrupts off so a wake can't slip between the load of *uaddr and the enqueue.
enum {
	FUTEX_WAIT = 0, FUTEX_WAKE = 1, FUTEX_REQUEUE = 3, FUTEX_CMP_REQUEUE = 4,
	FUTEX_WAIT_BITSET = 9, FUTEX_WAKE_BITSET = 10,
	FUTEX_PRIVATE_FLAG = 128, FUTEX_CLOCK_REALTIME = 256,
};

// futex is a RAW Linux-ABI syscall: its negated-errno return is read DIRECTLY by its low-level
// callers (the vendored musl pthread internals, Task 4; the picolibc lock retarget, Task 3.2),
// which compare it against musl's Linux-numbered errno constants — it is NOT routed through
// picolibc `errno`. So these two use Linux i386 numbers (musl's), deliberately distinct from the
// picolibc-aligned socket/file errnos in Syscall.h. EAGAIN(11)/EINVAL(22) happen to match both.
enum { ENOSYS = 38, ETIMEDOUT = 110 };

// The single kernel-global futex table. Its only state is a zeroed bucket array, identical to
// .bss zero-init — so it is correct even though the kernel never runs global constructors.
static FutexTable g_futex;

// Pop up to `n` waiters matching the key (optionally a bitset) and Scheduler::wake each. The
// pop accessor hands us the node so we can reach its Task*; the count is the return value.
static int futexWakeN(const void* space, void* uaddr, unsigned n, unsigned bitset, bool useBitset) {
	int woke = 0;
	for (unsigned i = 0; i < n; i++) {
		FutexWaiter* w = useBitset ? g_futex.popOneBitset(space, uaddr, bitset)
		                           : g_futex.popOne(space, uaddr);
		if (!w) break;
		w->woken = true;            // mark explicit wake so the waiter won't misread a late tick as timeout
		Scheduler::wake(w->task);
		woke++;
	}
	return woke;
}

// Kernel-callable FUTEX_WAKE: wake up to `n` waiters on (space, uaddr). Used by the
// CLONE_CHILD_CLEARTID handshake in procThreadExit (a joiner is parked here via pthread_join).
void futexWakeAddr(const void* space, void* uaddr, int n) {
	futexWakeN(space, uaddr, (unsigned) n, 0, false);
}

// Kernel-callable: evict every futex waiter owned by task `t` (within `space`) from the table.
// Called before reaping a thread's kernel stack (execve sibling-teardown / exit_group) so a bucket
// never keeps a pointer into the freed kstack the FutexWaiter node lived on.
void futexRemoveTask(const void* space, Task* t) {
	g_futex.removeTask(space, t);
}

static int futexSyscall(unsigned uaddr, int op, unsigned val, unsigned timeout,
		unsigned uaddr2, unsigned val3) {
	int cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
	if (!(op & FUTEX_PRIVATE_FLAG))
		return -ENOSYS;        // process-shared futexes are out of scope
	if (uaddr == 0) return -EINVAL;
	const void* space = ProcTable::current()->space;
	void* ua = (void*) uaddr;

	switch (cmd) {
	case FUTEX_WAIT:
	case FUTEX_WAIT_BITSET: {
		unsigned bitset = (cmd == FUTEX_WAIT_BITSET) ? val3 : 0;
		if (cmd == FUTEX_WAIT_BITSET && bitset == 0) return -EINVAL;   // empty mask is invalid
		// Atomic compare-and-enqueue: re-test *uaddr and park, interrupts off, so a concurrent
		// wake cannot land between the test and the enqueue (uniprocessor: cli is sufficient).
		unsigned long f = arch::cpuIrqSave();
		if (futexWaitPrecheck((volatile const unsigned*) uaddr, val) != 0) {
			arch::cpuIrqRestore(f);
			return -EAGAIN;    // the word already changed -> don't block
		}
		FutexWaiter w{};
		w.task = Scheduler::current();
		w.bitset = bitset;
		g_futex.enqueue(space, ua, &w);
		arch::cpuIrqRestore(f);

		// Block until woken. NULL timeout (timeout==0) blocks forever; otherwise treat the user
		// timespec* as a relative wait and arm a tick deadline so the wait can never hang. (Linux
		// makes WAIT_BITSET's timeout absolute; we approximate it as relative — best-effort.)
		bool timed = (timeout != 0);
		unsigned deadline = 0;
		if (timed) {
			const unsigned* ts = (const unsigned*) timeout;        // {tv_sec, tv_nsec} (i386)
			unsigned ms = ts[0] * 1000u + (ts[1] + 999999u) / 1000000u;
			deadline = Scheduler::ticks() + ms;
			Scheduler::sleepUntil(deadline);
		} else {
			Scheduler::block();
		}

		// Cancel our waiter (idempotent: a wake already unlinked it; on timeout/signal it is
		// still queued and this removes it) before deciding the outcome.
		unsigned long f2 = arch::cpuIrqSave();
		g_futex.remove(&w);
		arch::cpuIrqRestore(f2);

		if (w.woken) return 0;                                     // an explicit FUTEX_WAKE always wins,
		                                                           // even if the deferred wake let the
		                                                           // clock pass the deadline first
		if (hasPendingSignalCurrent()) return -ERESTARTSYS;        // restart or -EINTR at delivery
		if (timed && (int) (Scheduler::ticks() - deadline) >= 0) return -ETIMEDOUT;
		return 0;
	}
	case FUTEX_WAKE:
		return futexWakeN(space, ua, val, 0, false);
	case FUTEX_WAKE_BITSET:
		return futexWakeN(space, ua, val, val3, true);
	case FUTEX_REQUEUE:
	case FUTEX_CMP_REQUEUE: {
		// CMP_REQUEUE gates on *uaddr == val3 (the expected word) before touching the queue.
		if (cmd == FUTEX_CMP_REQUEUE) {
			unsigned long f = arch::cpuIrqSave();
			int mismatch = (futexWaitPrecheck((volatile const unsigned*) uaddr, val3) != 0);
			arch::cpuIrqRestore(f);
			if (mismatch) return -EAGAIN;
		}
		// Wake up to `val`, then requeue up to val2 (which Linux passes in the `timeout` slot
		// for REQUEUE) of the remaining waiters from uaddr onto uaddr2. Return woken + moved.
		int woke = futexWakeN(space, ua, val, 0, false);
		int moved = g_futex.requeue(space, ua, space, (void*) uaddr2, 0, (int) timeout);
		return woke + moved;
	}
	default:
		return -EINVAL;
	}
}

// MI syscall dispatch: map a syscall number + args to the Syscalls core. The
// arch trap (int 0x80 on x86) decodes registers and calls this.
int kernelSyscall(int nr, unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned a4,
		unsigned a5, arch::TrapFrame* tf) {
	int ret = -38;   // -ENOSYS
	Syscalls* g_sys = ProcTable::current()->sys;   // the running process's syscall state
	switch (nr) {
	case SYS_exit:
		// A non-last thread of a multithreaded process exits just THIS thread (CLEARTID wake +
		// reap), leaving the process alive for its siblings. The last thread falls through to the
		// normal path: set the exited flag and let the arch syscall-return hook run procExit().
		if (ProcTable::current()->threadCount > 1)
			procThreadExit((int) a0);   // does not return
		g_sys->exit((int) a0);
		ret = 0;
		break;
	case SYS_exit_group:
		procExitGroup((int) a0);   // terminate the whole group; does not return
		ret = 0;                   // unreachable
		break;
	case SYS_fork:
		ret = forkProcess(tf);
		break;
	case SYS_clone:
		// i386 ABI: a0=flags, a1=child_stack, a2=ptid, a3=tls, a4=ctid.
		ret = cloneThread(tf, a0, a1, a2, a3, a4);
		break;
	case SYS_getpid:
		ret = ProcTable::current()->pid;
		break;
	case SYS_getppid:
		ret = ProcTable::current()->parent;
		break;
	case SYS_times: {
		// a0 = struct tms* {utime, stime, cutime, cstime} as four 32-bit clock_t (ticks).
		// Fill it from the running process's real CPU accounting; child times are 0 (we
		// don't aggregate reaped children yet). Returns the monotonic tick count.
		Process* p = ProcTable::current();
		if (a0) {
			unsigned* t = (unsigned*) a0;
			t[0] = p ? p->utime : 0;   // tms_utime
			t[1] = p ? p->stime : 0;   // tms_stime
			t[2] = 0;                  // tms_cutime (no child-time accounting)
			t[3] = 0;                  // tms_cstime
		}
		ret = (int) Scheduler::ticks();
		break;
	}
	case SYS_waitpid:
		ret = waitProcess((int) a0, (int*) a1, (int) a2);   // a2 = options (WNOHANG/WUNTRACED)
		break;
	case SYS_kill:
		ret = signalSend((int) a0, (int) a1);
		break;
	case SYS_tkill:
		// i386: a0=tid, a1=sig. No tgid check (tgid = -1 = "any group").
		ret = signalSendThread(-1, (int) a0, (int) a1);
		break;
	case SYS_tgkill:
		// i386: a0=tgid, a1=tid, a2=sig.
		ret = signalSendThread((int) a0, (int) a1, (int) a2);
		break;
	case SYS_setpgid:
		ret = sysSetpgid((int) a0, (int) a1);
		break;
	case SYS_getpgid:
		ret = sysGetpgid((int) a0);
		break;
	case SYS_getpgrp:
		ret = sysGetpgid(0);                     // getpgrp() == getpgid(0)
		break;
	case SYS_setsid:
		ret = sysSetsid();
		break;
	case SYS_getsid:
		ret = sysGetsid((int) a0);
		break;
	case SYS_signal:
		ret = signalAction((int) a0, a1, a2);   // a1 = handler, a2 = sa_restorer
		break;
	case SYS_sigprocmask:
		ret = signalMask((int) a0, a1, (unsigned*) a2);   // legacy single-word form (low 31 signals)
		break;
	case SYS_rt_sigprocmask:
		// i386: a0=how, a1=set*, a2=oldset*, a3=sigsetsize (must be 8).
		ret = signalMaskRt((int) a0, (const uint64_t*) a1, (uint64_t*) a2, a3);
		break;
	case SYS_rt_sigaction:
		// i386: a0=sig, a1=act*, a2=old*, a3=sigsetsize (must be 8).
		ret = signalActionRt((int) a0, (const k_sigaction*) a1, (k_sigaction*) a2, a3);
		break;
	case SYS_rt_sigpending:
		// i386: a0=set*, a1=sigsetsize (must be 8).
		ret = signalPendingRt((uint64_t*) a0, a1);
		break;
	case SYS_pause:
		ret = signalPause();           // block until a signal -> -EINTR
		break;
	case SYS_sigsuspend:
		ret = signalSuspend(a0);       // legacy: a0 = wait-mask (single-word sigset)
		break;
	case SYS_rt_sigsuspend:
		// i386: a0=mask*, a1=sigsetsize (must be 8).
		ret = signalSuspendRt((const uint64_t*) a0, a1);
		break;
	case SYS_sigreturn:
		ret = signalReturn(tf);   // restores the trap frame; ret = the saved eax
		break;
	case SYS_read: {
		// Background tty read: a process not in the terminal's foreground group reading its
		// controlling tty is stopped with SIGTTIN (POSIX), unless its group is orphaned (-EIO).
		// The console is a job-control tty too: its foreground pgrp lives in the kernel
		// singleton (consoleGetPgrp), other ttys answer via their device (ttyPgrp).
		int fgpg = g_sys->isConsoleFd((int) a0) ? consoleGetPgrp() : g_sys->ttyPgrp((int) a0);
		Process* me = ProcTable::current();
		if (fgpg > 0 && me && fgpg != me->pgid) {
			if (ProcTable::isOrphanedGroup(me->pgid)) { ret = -5; break; }   // -EIO
			signalSendGroup(me->pgid, SIGTTIN);
			ret = -ERESTARTSYS;
			break;
		}
		ret = g_sys->read(a0, (void*) a1, a2);
		// A blocking read on an empty pipe or pty returns -EAGAIN; park on the object's wait
		// queue (event-driven: the writer wakes us) until data/EOF, or a signal interrupts.
		// Console blocking happens inside read() itself; an O_NONBLOCK fd returns -EAGAIN.
		while (ret == -EAGAIN && !g_sys->nonblock(a0)) {
			if (hasPendingSignalCurrent()) { ret = -ERESTARTSYS; break; }
			WaitQueue* wq = g_sys->fdWaitQueue(a0);
			if (wq) Scheduler::sleepOn(wq); else Scheduler::ioWait();
			ret = g_sys->read(a0, (void*) a1, a2);
		}
		if (ret > 0)                              // we drained bytes -> ring has space: wake writers
			Scheduler::wakeAll(g_sys->fdWaitQueue(a0));
		break;
	}
	case SYS_write: {
		// Loop until every byte is written, accumulating short (partial) writes from a pipe
		// or pty whose ring fills mid-write. -EAGAIN (ring completely full) blocks until the
		// reader drains; a signal or a non-blocking fd returns the partial count (or the bare
		// error if nothing was written yet). Userspace always sees a full write or a signal.
		unsigned done = 0;
		ret = 0;
		while (done < a2) {
			int r = g_sys->write(a0, (const char*) a1 + done, a2 - done);
			if (r == -EAGAIN) {
				if (g_sys->nonblock(a0)) { ret = done ? (int) done : -EAGAIN; break; }
				if (hasPendingSignalCurrent()) { ret = done ? (int) done : -ERESTARTSYS; break; }
				WaitQueue* wq = g_sys->fdWaitQueue(a0);
				if (wq) Scheduler::sleepOn(wq); else Scheduler::ioWait();
				continue;
			}
			if (r < 0) { ret = done ? (int) done : r; break; }
			done += (unsigned) r;
			ret = (int) done;
			Scheduler::wakeAll(g_sys->fdWaitQueue(a0));   // bytes landed -> wake the reader/peer
		}
		break;
	}
	case SYS_pipe:
		ret = g_sys->pipe((int*) a0);
		break;
	case SYS_dup:
		ret = g_sys->dup((int) a0);
		break;
	case SYS_dup2:
		ret = g_sys->dup2((int) a0, (int) a1);
		break;
	case SYS_poll: {
		// a0 = struct pollfd*, a1 = nfds, a2 = timeout ms (-1 = infinite, 0 = non-blocking).
		PollFd* pfds = (PollFd*) a0;
		int nfds = (int) a1;
		int timeout = (int) a2;
		ret = pollScanConsoleAware(g_sys, pfds, nfds);
		if (ret == 0 && timeout != 0) {
			unsigned start = Scheduler::ticks();
			for (;;) {
				if (hasPendingSignalCurrent()) { ret = -ERESTARTSYS; break; }
				if (timeout > 0 && Scheduler::ticks() - start >= (unsigned) timeout) {
					ret = 0; break;            // timed out, nothing ready
				}
				Scheduler::ioWait();
				int r = pollScanConsoleAware(g_sys, pfds, nfds);
				if (r > 0) { ret = r; break; }
			}
		}
		break;
	}
	case SYS__newselect:
		// a0=nfds, a1=readfds*, a2=writefds*, a3=exceptfds*, a4=timeval* (NULL = block forever).
		ret = doSelect(g_sys, (int) a0, (unsigned*) a1, (unsigned*) a2, (unsigned*) a3, (unsigned*) a4);
		break;
	case SYS_socketcall: {
		// a0 = sub-call number, a1 = pointer to its argument array (each entry is 4 bytes).
		const unsigned* uargs = (const unsigned*) a1;
		if (!uargs) { ret = -EINVAL; break; }
		unsigned A[6] = {0,0,0,0,0,0};
		for (int i = 0; i < 6; i++) A[i] = uargs[i];   // over-reads are harmless (page-resident args)
		ret = socketOp(g_sys, (int) a0, A);
		break;
	}
	// Direct socket syscalls (Linux >=4.3 i386). a5 (ebp) carries the 6th arg for sendto/recvfrom.
	case SYS_socket:      { unsigned A[6] = {a0,a1,a2,0,0,0};      ret = socketOp(g_sys, SC_SOCKET, A); break; }
	case SYS_bind:        { unsigned A[6] = {a0,a1,a2,0,0,0};      ret = socketOp(g_sys, SC_BIND, A); break; }
	case SYS_connect:     { unsigned A[6] = {a0,a1,a2,0,0,0};      ret = socketOp(g_sys, SC_CONNECT, A); break; }
	case SYS_listen:      { unsigned A[6] = {a0,a1,0,0,0,0};       ret = socketOp(g_sys, SC_LISTEN, A); break; }
	case SYS_accept4:     { unsigned A[6] = {a0,a1,a2,a3,0,0};     ret = socketOp(g_sys, SC_ACCEPT4, A); break; }
	case SYS_getsockname: { unsigned A[6] = {a0,a1,a2,0,0,0};      ret = socketOp(g_sys, SC_GETSOCKNAME, A); break; }
	case SYS_getpeername: { unsigned A[6] = {a0,a1,a2,0,0,0};      ret = socketOp(g_sys, SC_GETPEERNAME, A); break; }
	case SYS_socketpair:  { unsigned A[6] = {a0,a1,a2,a3,0,0};     ret = socketOp(g_sys, SC_SOCKETPAIR, A); break; }
	case SYS_setsockopt:  { unsigned A[6] = {a0,a1,a2,a3,a4,0};    ret = socketOp(g_sys, SC_SETSOCKOPT, A); break; }
	case SYS_getsockopt:  { unsigned A[6] = {a0,a1,a2,a3,a4,0};    ret = socketOp(g_sys, SC_GETSOCKOPT, A); break; }
	case SYS_sendto:      { unsigned A[6] = {a0,a1,a2,a3,a4,a5};   ret = socketOp(g_sys, SC_SENDTO, A); break; }
	case SYS_recvfrom:    { unsigned A[6] = {a0,a1,a2,a3,a4,a5};   ret = socketOp(g_sys, SC_RECVFROM, A); break; }
	case SYS_sendmsg:     { unsigned A[6] = {a0,a1,a2,0,0,0};      ret = socketOp(g_sys, SC_SENDMSG, A); break; }
	case SYS_recvmsg:     { unsigned A[6] = {a0,a1,a2,0,0,0};      ret = socketOp(g_sys, SC_RECVMSG, A); break; }
	case SYS_shutdown:    { unsigned A[6] = {a0,a1,0,0,0,0};       ret = socketOp(g_sys, SC_SHUTDOWN, A); break; }
	case SYS_open:
		ret = g_sys->open(String((char*) a0), a1);
		break;
	case SYS_close: {
		// Wake anyone blocked on this object after dropping the fd, so a peer reading a pipe
		// whose last writer just closed sees EOF (and a writer sees EPIPE) instead of sleeping
		// forever. BUT if this was the pipe's last end, close() frees the Pipe (and its embedded
		// WaitQueue), so `wq` would dangle — skip the wake then. That is always safe: a pipe with
		// no open ends can have no blocked waiter (a blocked reader/writer holds an end open).
		WaitQueue* wq = g_sys->fdWaitQueue(a0);
		bool freedShared = false;
		ret = g_sys->close(a0, &freedShared);
		if (!freedShared)
			Scheduler::wakeAll(wq);
		break;
	}
	case SYS_unlink:
		ret = g_sys->unlink(String((char*) a0));
		break;
	case SYS_mkdir:
		ret = g_sys->mkdir(String((char*) a0), (int) a1);
		break;
	case SYS_rmdir:
		ret = g_sys->rmdir(String((char*) a0));
		break;
	case SYS_rename:
		ret = g_sys->rename(String((char*) a0), String((char*) a1));
		break;
	case SYS_link:
		ret = g_sys->link(String((char*) a0), String((char*) a1));
		break;
	case SYS_symlink:
		ret = g_sys->symlink(String((char*) a0), String((char*) a1));
		break;
	case SYS_chmod:
		ret = g_sys->chmod(String((char*) a0), (int) a1);
		break;
	case SYS_fchmod:
		ret = g_sys->fchmod((int) a0, (int) a1);
		break;
	case SYS_chown:
		ret = g_sys->chown(String((char*) a0), (int) a1, (int) a2);
		break;
	case SYS_lchown:
		ret = g_sys->lchown(String((char*) a0), (int) a1, (int) a2);
		break;
	case SYS_fchown:
		ret = g_sys->fchown((int) a0, (int) a1, (int) a2);
		break;
	case SYS_truncate:
		ret = g_sys->truncate(String((char*) a0), a1);
		break;
	case SYS_ftruncate:
		ret = g_sys->ftruncate((int) a0, a1);
		break;
	case SYS_utimes:
		ret = g_sys->utimes(String((char*) a0), a1, a2);
		break;
	case SYS_access:
		ret = g_sys->access(String((char*) a0), (int) a1);
		break;
	case SYS_faccessat:
		ret = g_sys->faccessat((int) a0, String((char*) a1), (int) a2, (int) a3);
		break;
	case SYS_utime:
		ret = g_sys->utime(String((char*) a0), (const void*) a1);
		break;
	case SYS_utimensat:
		ret = g_sys->utimensat((int) a0, String((char*) a1), (const void*) a2, (int) a3);
		break;
	case SYS_statfs:
		ret = g_sys->statfs(String((char*) a0), (void*) a1);
		break;
	case SYS_fstatfs:
		ret = g_sys->fstatfs((int) a0, (void*) a1);
		break;
	case SYS_fsync:
	case SYS_fdatasync:
		ret = g_sys->fsync((int) a0);
		break;
	case SYS_sync:
		ret = 0;
		break;
	case SYS_umask:
		ret = g_sys->umask((int) a0);
		break;
	case SYS_fchdir:
		ret = g_sys->fchdir((int) a0);
		break;
	case SYS_creat:
		ret = g_sys->creat(String((char*) a0), (int) a1);
		break;
	case SYS_openat:
		ret = g_sys->openat((int) a0, String((char*) a1), (int) a2);
		break;
	case SYS_mkdirat:
		ret = g_sys->mkdirat((int) a0, String((char*) a1), (int) a2);
		break;
	case SYS_unlinkat:
		ret = g_sys->unlinkat((int) a0, String((char*) a1), (int) a2);
		break;
	case SYS_renameat:
		ret = g_sys->renameat((int) a0, String((char*) a1), (int) a2, String((char*) a3));
		break;
	case SYS_renameat2:
		ret = g_sys->renameat2((int) a0, String((char*) a1), (int) a2, String((char*) a3), (int) a4);
		break;
	case SYS_linkat:
		ret = g_sys->linkat((int) a0, String((char*) a1), (int) a2, String((char*) a3), (int) a4);
		break;
	case SYS_symlinkat:
		ret = g_sys->symlinkat(String((char*) a0), (int) a1, String((char*) a2));
		break;
	case SYS_readlinkat:
		ret = g_sys->readlinkat((int) a0, String((char*) a1), (char*) a2, a3);
		break;
	case SYS_fchmodat:
		ret = g_sys->fchmodat((int) a0, String((char*) a1), (int) a2, (int) a3);
		break;
	case SYS_fchownat:
		ret = g_sys->fchownat((int) a0, String((char*) a1), (int) a2, (int) a3, (int) a4);
		break;
	case SYS_fstatat64:
		ret = g_sys->fstatat((int) a0, String((char*) a1), (LinuxStat*) a2, (int) a3);
		break;
	case SYS_getuid: case SYS_getuid32:
		ret = g_sys->getuid();
		break;
	case SYS_geteuid: case SYS_geteuid32:
		ret = g_sys->geteuid();
		break;
	case SYS_getgid: case SYS_getgid32:
		ret = g_sys->getgid();
		break;
	case SYS_getegid: case SYS_getegid32:
		ret = g_sys->getegid();
		break;
	case SYS_setuid: case SYS_setuid32:
		ret = g_sys->setuid((int) a0);
		break;
	case SYS_setgid: case SYS_setgid32:
		ret = g_sys->setgid((int) a0);
		break;
	case SYS_chdir:
		ret = g_sys->chdir(String((char*) a0));
		break;
	case SYS_getcwd:
		ret = g_sys->getcwd((char*) a0, a1);
		break;
	case SYS_lseek:
		ret = g_sys->lseek(a0, a1, a2);
		break;
	case SYS_stat:
		ret = g_sys->stat(String((char*) a0), (LinuxStat*) a1);
		break;
	case SYS_lstat:
		ret = g_sys->lstat(String((char*) a0), (LinuxStat*) a1);
		break;
	case SYS_readlink:
		// a0 = path, a1 = buf, a2 = bufsize. Returns byte count (no NUL) or -errno.
		ret = g_sys->readlink(String((char*) a0), (char*) a1, a2);
		break;
	case SYS_fstat:
		ret = g_sys->fstat(a0, (LinuxStat*) a1);
		break;
	case SYS_getdents64:
		ret = g_sys->getdents64(a0, (void*) a1, a2);
		break;
	case SYS_ioctl:
		// FIONBIO (0x5421): the BSD/Windows way to set/clear non-blocking mode on a descriptor
		// (OpenSSL's BIO uses it on sockets). Map it to the same O_NONBLOCK status bit fcntl(F_SETFL)
		// toggles, so a later connect/recv on the fd honours it. Works on any fd, like Linux.
		if (a1 == 0x5421) {
			if (!a2) { ret = -EFAULT; break; }
			ret = g_sys->fcntl((int) a0, F_SETFL, *(int*) a2 ? O_NONBLOCK : 0);
			break;
		}
		// Network interface ioctls (SIOC*, 0x89xx) on a socket fd -> the net layer (ifconfig/DHCP).
		if (a1 >= 0x8900 && a1 <= 0x89ff && g_sys->isSocketFd((int) a0)) {
			ret = g_sys->netIoctl((int) a0, a1, (void*) a2);
			break;
		}
		// The console's foreground-process-group ioctls (tcgetpgrp/tcsetpgrp) are job-control
		// state of the physical terminal, not of any one fd-table -> handled here against the
		// kernel singleton (Syscalls::ioctl would only -EINVAL them). This makes the console a
		// real job-control tty, exactly like the pty's Pty::ioctl.
		if (g_sys->isConsoleFd((int) a0) &&
				(a1 == IOCTL_TIOCGPGRP || a1 == IOCTL_TIOCSPGRP)) {
			if (!a2) { ret = -EINVAL; break; }
			if (a1 == IOCTL_TIOCGPGRP) { *(int*) a2 = consoleGetPgrp(); ret = 0; }
			else                       { consoleSetPgrp(*(int*) a2);    ret = 0; }
			break;
		}
		// TIOCGWINSZ on the console reports the framebuffer/VGA grid, so a full-screen TUI (vim)
		// uses the whole screen instead of falling back to terminfo's 80x24 default.
		if (g_sys->isConsoleFd((int) a0) && a1 == IOCTL_TIOCGWINSZ) {
			if (!a2) { ret = -EINVAL; break; }
			unsigned cols = 80, rows = 25;
			arch::consoleSize(&cols, &rows);
			Winsize* ws = (Winsize*) a2;
			ws->ws_row = (unsigned short) rows;
			ws->ws_col = (unsigned short) cols;
			ws->ws_xpixel = ws->ws_ypixel = 0;
			ret = 0;
			break;
		}
		ret = g_sys->ioctl((int) a0, a1, (void*) a2);
		// A successful TCSETS on the console must take effect: drive the line discipline
		// from the new termios (canonical -> cooked, ICANON cleared -> raw), so tcsetattr()
		// actually switches input modes (the same mechanism SYS_termmode uses directly).
		if (ret == 0 && g_sys->isConsoleFd((int) a0) &&
				(a1 == IOCTL_TCSETS || a1 == IOCTL_TCSETSW || a1 == IOCTL_TCSETSF))
			arch::inputSetRaw(g_sys->consoleRaw() ? 1 : 0);
		break;
	case SYS_fcntl:
		ret = g_sys->fcntl((int) a0, (int) a1, (int) a2);
		break;
	case SYS_mmap2: {
		// ABI (libc mmap wrapper): a0 = length, a1 = prot, a2 = flags, a3 = fd, a4 = offset.
		// Three kinds: a device region (e.g. /dev/fb0) mapped to its physical pages; an
		// anonymous mapping (fd < 0) of fresh zeroed pages; and a file-backed mapping (a
		// regular-file fd) of zeroed pages eagerly filled from the file. Returns the user VA.
		unsigned length = a0;
		int prot = (int) a1;
		int fd = (int) a3;
		unsigned offset = a4;
		Process* p = ProcTable::current();
		arch::AddressSpace* space = (arch::AddressSpace*) p->space;
		if (fd >= 0) {                          // device region? (fb0 etc.)
			unsigned phys = 0, dlen = 0;
			if (g_sys->mmapInfo(fd, &phys, &dlen) >= 0) {
				unsigned want = (length && length < dlen) ? length : dlen;
				unsigned va = arch::mmuMapUserFb(space, phys, want);
				ret = va ? (int) va : -12;   // -ENOMEM
				break;
			}
		}
		if (length == 0) { ret = -22; break; }  // -EINVAL
		if (p->mmapNext == 0)
			p->mmapNext = arch::mmuMmapBase();
		unsigned bytes = (length + 0xFFFu) & ~0xFFFu;
		if (p->mmapNext + bytes > arch::mmuMmapMax()) { ret = -12; break; }   // window full
		unsigned va = p->mmapNext;
		// File-backed must be writable so we can load into it; anonymous honors PROT_WRITE (2).
		int writable = (fd >= 0) ? 1 : ((prot & 2) != 0);
		if (arch::mmuMapAnon(space, va, bytes, writable) != 0) { ret = -12; break; }
		p->mmapNext = va + bytes;
		if (fd >= 0) {                          // file-backed: eager-read the file into the map
			g_sys->lseek(fd, (int) offset, 0 /*SEEK_SET*/);
			unsigned got = 0;
			while (got < length) {
				int r = g_sys->read(fd, (char*) (va + got), length - got);
				if (r <= 0) break;              // EOF or error: leave the rest zero-filled
				got += (unsigned) r;
			}
		}
		ret = (int) va;
		break;
	}
	case SYS_execve: {
		// Deep-copy path + argv + envp from the caller's (currently active) user space into
		// kernel buffers before execve swaps CR3 to the kernel directory to stage and
		// load the new image (after which the caller's user pointers are unmapped).
		static char pathBuf[256];
		static char strBuf[ARG_STRBYTES];          // packed argv+envp strings (shared budget)
		static const char* argPtrs[ARG_MAXVEC + 1];
		static const char* envPtrs[ARG_MAXVEC + 1];
		copyStr(pathBuf, (const char*) a0, sizeof pathBuf);
		int used = 0;
		int argc = copyVec((const char* const*) a1, argPtrs, strBuf, &used);
		if (argc < 0) { ret = argc; break; }       // -E2BIG: too many/too-long arguments
		argPtrs[argc] = 0;
		int envc = copyVec((const char* const*) a2, envPtrs, strBuf, &used);
		if (envc < 0) { ret = envc; break; }        // -E2BIG: environment too large
		envPtrs[envc] = 0;
		ret = execve(g_vfs, pathBuf, argPtrs, argc, envPtrs, envc, tf);   // rewrites tf, no return
		break;
	}
	case SYS_brk: {
		// Linux brk: a0 == 0 queries the current break; otherwise set it, clamped to
		// [brkBase, brkMax]. On success return the new break; on any failure return the
		// CURRENT (unchanged) break (glibc's sbrk detects failure by comparing).
		Process* p = ProcTable::current();
		arch::AddressSpace* space = (arch::AddressSpace*) p->space;
		unsigned req = a0;
		if (req != 0 && req >= p->brkBase && req <= p->brkMax && space) {
			if (arch::mmuSetUserBrk(space, p->brkCur, req) == 0)
				p->brkCur = req;
		}
		ret = (int) p->brkCur;
		break;
	}
	case SYS_termmode:
		arch::inputSetRaw((int) a0);
		ret = 0;
		break;
	case SYS_reboot:
		arch::powerOff();          // does not return
		ret = 0;
		break;
	case SYS_getrandom: {
		// getrandom(buf, count, flags). a0=buf, a1=count, a2=flags. Our CSPRNG never blocks and is
		// always seeded, so GRND_NONBLOCK/GRND_RANDOM are safely ignored — we always return the full
		// count of CSPRNG bytes (the kernel ran csprngKernelSeed() before userspace started).
		if (a0 == 0) { ret = -EFAULT; break; }
		csprngBytes((void*) a0, a1);
		ret = (int) a1;
		break;
	}
	case SYS_clock_gettime:
		// a0 = clk_id, a1 = user struct timespec*. Pass the BOOT epoch (RTC sampled once at
		// boot), not the live RTC: clockGettime derives both seconds and sub-seconds from the
		// monotonic tick and offsets realtime by this base, so the clock stays monotonic
		// (a live-RTC read would desync at second boundaries — negative ping RTTs). MONOTONIC
		// ignores the base. User space is active.
		ret = g_sys->clockGettime((int) a0, Scheduler::ticks(), kernel::bootEpochSeconds(),
				(KTimespec*) a1);
		break;
	case SYS_nanosleep: {
		// a0 = req timespec*, a1 = rem timespec* (optional). Block until the deadline,
		// waking on each timer tick; a signal for us aborts early (restart or -EINTR is
		// decided at delivery, exactly like a blocking read).
		unsigned ms = g_sys->nanosleepMs((const KTimespec*) a0);
		unsigned start = Scheduler::ticks();
		unsigned deadline = start + ms;
		ret = 0;
		while (Scheduler::ticks() - start < ms) {
			if (hasPendingSignalCurrent()) {
				if (a1) {   // report the unslept remainder
					unsigned done = Scheduler::ticks() - start;
					unsigned left = done < ms ? ms - done : 0;
					// Convert the unslept ms into a duration timespec: monotonic (no epoch).
					g_sys->clockGettime(1 /*MONOTONIC*/, left, 0, (KTimespec*) a1);
				}
				ret = -ERESTARTSYS;
				break;
			}
			// Sleep straight to the deadline: ONE wakeup for the whole sleep, not one per tick
			// (a signal wake returns early and the loop re-checks above).
			Scheduler::sleepUntil(deadline);
		}
		break;
	}
	case SYS_futex:
		// a0=uaddr, a1=op, a2=val, a3=timeout (or val2 for REQUEUE), a4=uaddr2, a5=val3.
		ret = futexSyscall(a0, (int) a1, a2, a3, a4, a5);
		break;
	case SYS_gettid: {
		// The calling thread's tid (== pid for the leader). Thread tracking is live after
		// Task 0.2; guard the null case defensively (return the pid as a sane fallback).
		Thread* t = ProcTable::currentThread();
		ret = t ? t->tid : ProcTable::current()->pid;
		break;
	}
	case SYS_set_thread_area: {
		// a0 = struct user_desc*. The kernel runs on the caller's address space, so the user
		// pointer is read directly (same pattern as futex's uaddr / select's fd_sets). NanOS
		// has ONE fixed TLS slot (GDT entry 6, selector 0x33): point it at the caller's TLS
		// block. userDescToSelector writes entry_number=6 back (musl derives %gs from it).
		UserDesc* ud = (UserDesc*) a0;
		if (!ud) { ret = -EINVAL; break; }
		Thread* t = ProcTable::currentThread();
		if (!t) { ret = -EINVAL; break; }
		userDescToSelector(ud);                 // writes ud->entry_number = 6
		t->tlsBase = ud->base_addr;
		arch::archLoadThreadTls(ud->base_addr); // re-point entry 6 + reload %gs now
		ret = 0;
		break;
	}
	case SYS_set_tid_address: {
		// a0 = clear-tid address. Record it on the calling thread; on thread exit (Task 2.3)
		// the kernel zeroes *ptr and futex-wakes it (the pthread_join handshake). Returns tid.
		Thread* t = ProcTable::currentThread();
		if (!t) { ret = ProcTable::current()->pid; break; }
		t->clearTidAddr = a0;
		ret = t->tid;
		break;
	}
	}
	return ret;
}

void installSyscalls(Vfs* vfs) {
	ProcTable::init();
	Process* p = ProcTable::alloc(0);          // pid 1: the boot/init process
	p->sys = new Syscalls(vfs, consoleSink);
	ProcTable::setCurrent(p);
	g_vfs = vfs;
	arch::syscallInit();
}

Syscalls* kernelSyscalls() {
	return ProcTable::current()->sys;
}

} /* namespace kernel */
