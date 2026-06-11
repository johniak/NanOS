#include "SyscallDispatch.h"
#include "Syscall.h"
#include "Process.h"
#include "Exec.h"
#include "SignalDispatch.h"
#include "Scheduler.h"
#include <arch/syscall.h>
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

// MI syscall dispatch: map a syscall number + args to the Syscalls core. The
// arch trap (int 0x80 on x86) decodes registers and calls this.
int kernelSyscall(int nr, unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned a4,
		arch::TrapFrame* tf) {
	int ret = -38;   // -ENOSYS
	Syscalls* g_sys = ProcTable::current()->sys;   // the running process's syscall state
	switch (nr) {
	case SYS_exit:
		g_sys->exit((int) a0);
		ret = 0;
		break;
	case SYS_fork:
		ret = forkProcess(tf);
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
		ret = signalMask((int) a0, a1, (unsigned*) a2);
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
	case SYS_open:
		ret = g_sys->open(String((char*) a0), a1);
		break;
	case SYS_close: {
		// Wake anyone blocked on this object before/after dropping the fd, so a peer reading a
		// pipe whose last writer just closed sees EOF (and a writer sees EPIPE) instead of
		// sleeping forever. The queue lives on the shared object, which outlives this fd.
		WaitQueue* wq = g_sys->fdWaitQueue(a0);
		ret = g_sys->close(a0);
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
		ret = g_sys->access(String((char*) a1), (int) a2);   // dirfd a0 ignored for absolute paths
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
	case SYS_renameat2:
		ret = g_sys->renameat((int) a0, String((char*) a1), (int) a2, String((char*) a3));
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
	case SYS_clock_gettime:
		// a0 = clk_id, a1 = user struct timespec*. Pass the RTC wall-clock seconds so
		// CLOCK_REALTIME is real time; CLOCK_MONOTONIC ignores it. User space is active.
		ret = g_sys->clockGettime((int) a0, Scheduler::ticks(), arch::rtcEpoch(),
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
