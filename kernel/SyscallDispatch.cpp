#include "SyscallDispatch.h"
#include "Syscall.h"
#include "Process.h"
#include "Exec.h"
#include "SignalDispatch.h"
#include "Scheduler.h"
#include <arch/syscall.h>
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
	case SYS_waitpid:
		ret = waitProcess((int) a0, (int*) a1, (int) a2);   // a2 = options (WNOHANG/WUNTRACED)
		break;
	case SYS_kill:
		ret = signalSend((int) a0, (int) a1);
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
	case SYS_read:
		ret = g_sys->read(a0, (void*) a1, a2);
		// A blocking pipe read on an empty pipe returns -EAGAIN; wait (waking each tick as
		// the writer is scheduled) until data/EOF, or a signal interrupts. Console blocking
		// happens inside read() itself; O_NONBLOCK returns -EAGAIN to the caller.
		while (ret == -EAGAIN && g_sys->isPipe(a0) && !g_sys->nonblock(a0)) {
			if (hasPendingSignalCurrent()) { ret = -ERESTARTSYS; break; }
			arch::halt_or_hlt();
			ret = g_sys->read(a0, (void*) a1, a2);
		}
		break;
	case SYS_write:
		ret = g_sys->write(a0, (const void*) a1, a2);
		while (ret == -EAGAIN && g_sys->isPipe(a0) && !g_sys->nonblock(a0)) {
			if (hasPendingSignalCurrent()) { ret = -ERESTARTSYS; break; }
			arch::halt_or_hlt();
			ret = g_sys->write(a0, (const void*) a1, a2);
		}
		break;
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
				arch::halt_or_hlt();
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
		// Deep-copy path + argv from the caller's (currently active) user space into
		// kernel buffers before execve swaps CR3 to the kernel directory to stage and
		// load the new image (after which the caller's user pointers are unmapped).
		static char pathBuf[256];
		static char argBuf[16][128];
		static const char* argPtrs[17];
		copyStr(pathBuf, (const char*) a0, sizeof pathBuf);
		const char* const* uargv = (const char* const*) a1;
		int argc = 0;
		if (uargv)
			for (; argc < 16 && uargv[argc]; argc++)
				copyStr(argBuf[argc], uargv[argc], sizeof argBuf[argc]);
		for (int i = 0; i < argc; i++)
			argPtrs[i] = argBuf[i];
		argPtrs[argc] = 0;
		ret = execve(g_vfs, pathBuf, argPtrs, argc, tf);   // on success rewrites tf, no return here
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
		// a0 = clk_id, a1 = user struct timespec*. User space is active, so write through.
		ret = g_sys->clockGettime((int) a0, Scheduler::ticks(), (KTimespec*) a1);
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
					g_sys->clockGettime(0, left, (KTimespec*) a1);
				}
				ret = -ERESTARTSYS;
				break;
			}
			arch::halt_or_hlt();
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
