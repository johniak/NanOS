#include "SyscallDispatch.h"
#include "Syscall.h"
#include "Process.h"
#include "Exec.h"
#include "SignalDispatch.h"
#include <arch/syscall.h>
#include <arch/input.h>
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
		break;
	case SYS_write:
		ret = g_sys->write(a0, (const void*) a1, a2);
		break;
	case SYS_open:
		ret = g_sys->open(String((char*) a0), a1);
		break;
	case SYS_close:
		ret = g_sys->close(a0);
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
	case SYS_termmode:
		arch::inputSetRaw((int) a0);
		ret = 0;
		break;
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
