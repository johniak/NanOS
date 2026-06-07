#include "SyscallDispatch.h"
#include "Syscall.h"
#include <arch/syscall.h>
#include "Console.h"

namespace kernel {

static Syscalls* g_sys = 0;

static int consoleSink(const char* buf, unsigned len) {
	for (unsigned i = 0; i < len; i++)
		Console::write(buf[i]);
	return (int) len;
}

// MI syscall dispatch: map a syscall number + args to the Syscalls core. The
// arch trap (int 0x80 on x86) decodes registers and calls this.
int kernelSyscall(int nr, unsigned a0, unsigned a1, unsigned a2) {
	int ret = -38;   // -ENOSYS
	switch (nr) {
	case SYS_exit:
		g_sys->exit((int) a0);
		ret = 0;
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
	case SYS_fstat:
		ret = g_sys->fstat(a0, (LinuxStat*) a1);
		break;
	case SYS_getdents64:
		ret = g_sys->getdents64(a0, (void*) a1, a2);
		break;
	}
	return ret;
}

void installSyscalls(Vfs* vfs) {
	g_sys = new Syscalls(vfs, consoleSink);
	arch::syscallInit();
}

Syscalls* kernelSyscalls() {
	return g_sys;
}

} /* namespace kernel */
