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

// MI syscall dispatch: map a syscall number + args to the Syscalls core. The
// arch trap (int 0x80 on x86) decodes registers and calls this.
int kernelSyscall(int nr, unsigned a0, unsigned a1, unsigned a2, arch::TrapFrame* tf) {
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
		int fgpg = g_sys->ttyPgrp((int) a0);
		Process* me = ProcTable::current();
		if (fgpg > 0 && me && fgpg != me->pgid) {
			if (ProcTable::isOrphanedGroup(me->pgid)) { ret = -5; break; }   // -EIO
			signalSendGroup(me->pgid, SIGTTIN);
			ret = -ERESTARTSYS;
			break;
		}
		ret = g_sys->read(a0, (void*) a1, a2);
		// A blocking read on an empty pipe or pty returns -EAGAIN; wait (waking each tick as
		// the writer is scheduled) until data/EOF, or a signal interrupts. Console blocking
		// happens inside read() itself; an O_NONBLOCK fd returns -EAGAIN to the caller.
		while (ret == -EAGAIN && !g_sys->nonblock(a0)) {
			if (hasPendingSignalCurrent()) { ret = -ERESTARTSYS; break; }
			Scheduler::ioWait();
			ret = g_sys->read(a0, (void*) a1, a2);
		}
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
				Scheduler::ioWait();
				continue;
			}
			if (r < 0) { ret = done ? (int) done : r; break; }
			done += (unsigned) r;
			ret = (int) done;
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
		ret = g_sys->pollScan(pfds, nfds);
		if (ret == 0 && timeout != 0) {
			unsigned start = Scheduler::ticks();
			for (;;) {
				if (hasPendingSignalCurrent()) { ret = -ERESTARTSYS; break; }
				if (timeout > 0 && Scheduler::ticks() - start >= (unsigned) timeout) {
					ret = 0; break;            // timed out, nothing ready
				}
				Scheduler::ioWait();
				int r = g_sys->pollScan(pfds, nfds);
				if (r > 0) { ret = r; break; }
			}
		}
		break;
	}
	case SYS_open:
		ret = g_sys->open(String((char*) a0), a1);
		break;
	case SYS_close:
		ret = g_sys->close(a0);
		break;
	case SYS_unlink:
		ret = g_sys->unlink(String((char*) a0));
		break;
	case SYS_mkdir:
		ret = g_sys->mkdir(String((char*) a0), (int) a1);
		break;
	case SYS_lseek:
		ret = g_sys->lseek(a0, a1, a2);
		break;
	case SYS_stat:
		ret = g_sys->stat(String((char*) a0), (LinuxStat*) a1);
		break;
	case SYS_fstat:
		ret = g_sys->fstat(a0, (LinuxStat*) a1);
		break;
	case SYS_getdents64:
		ret = g_sys->getdents64(a0, (void*) a1, a2);
		break;
	case SYS_ioctl:
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
		// Simplified ABI: a0 = fd, a1 = length, a2 = offset (the libc mmap() wrapper
		// repacks the 6 POSIX args into these). We support mapping a device's region
		// (e.g. /dev/fb0) into the calling process. Returns the user VA, or <0 on error.
		unsigned phys = 0, len = 0;
		int r = g_sys->mmapInfo((int) a0, &phys, &len);
		if (r < 0) {
			ret = r;
			break;
		}
		unsigned want = (a1 && a1 < len) ? a1 : len;
		arch::AddressSpace* space = (arch::AddressSpace*) ProcTable::current()->space;
		unsigned va = arch::mmuMapUserFb(space, phys, want);
		ret = va ? (int) va : -12;   // -ENOMEM
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
			Scheduler::ioWait();
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
