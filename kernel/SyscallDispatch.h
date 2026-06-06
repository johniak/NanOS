/*
 * SyscallDispatch.h
 *
 * Kernel-only glue: installs the int 0x80 handler that maps Linux i386 syscall
 * numbers to the Syscalls core. Not host-tested (touches Registers/IDT);
 * verified in QEMU.
 */
#pragma once
#include "Vfs.h"
#include "Syscall.h"

namespace kernel {
void installSyscalls(Vfs* vfs);
Syscalls* kernelSyscalls();   // the installed Syscalls instance (0 if not yet installed)
}
