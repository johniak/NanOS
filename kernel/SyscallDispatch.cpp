#include "SyscallDispatch.h"
#include "Syscall.h"
#include "Interrupt.h"
#include "Console.h"

namespace kernel {

static Syscalls* g_sys = 0;

static int consoleSink(const char* buf, unsigned len) {
	for (unsigned i = 0; i < len; i++)
		Console::write(buf[i]);
	return (int) len;
}

// int 0x80 handler: Linux i386 ABI — nr in eax, args in ebx/ecx/edx/esi/edi,
// return in eax (propagates back to the caller through isr_common_stub's popa).
static void syscallDispatch(Registers* r) {
	int ret = -38;   // -ENOSYS
	switch (r->eax) {
	case SYS_exit:
		g_sys->exit((int) r->ebx);
		ret = 0;
		break;
	case SYS_read:
		ret = g_sys->read(r->ebx, (void*) r->ecx, r->edx);
		break;
	case SYS_write:
		ret = g_sys->write(r->ebx, (const void*) r->ecx, r->edx);
		break;
	case SYS_open:
		ret = g_sys->open(String((char*) r->ebx), r->ecx);
		break;
	case SYS_close:
		ret = g_sys->close(r->ebx);
		break;
	case SYS_lseek:
		ret = g_sys->lseek(r->ebx, r->ecx, r->edx);
		break;
	case SYS_fstat:
		ret = g_sys->fstat(r->ebx, (LinuxStat*) r->ecx);
		break;
	case SYS_getdents64:
		ret = g_sys->getdents64(r->ebx, (void*) r->ecx, r->edx);
		break;
	}
	r->eax = (unsigned) ret;
}

void installSyscalls(Vfs* vfs) {
	g_sys = new Syscalls(vfs, consoleSink);
	Interrupt::registerInterruptHandler(0x80, &syscallDispatch);
}

} /* namespace kernel */
