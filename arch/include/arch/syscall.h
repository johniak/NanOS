/*
 * arch/syscall.h — MI/MD contract for the syscall boundary.
 *
 * The arch decodes its trap (x86: int 0x80, nr in eax, args in ebx/ecx/edx) and
 * calls the MI dispatcher kernel::kernelSyscall; the MI side maps the number to
 * the Syscalls core. The numbers themselves (our chosen Linux i386 ABI) live in
 * kernel/Syscall.h and are MI.
 */
#pragma once
#include <stdint.h>

namespace arch { struct TrapFrame; }   // opaque trap frame (passed through for execve/fork)

namespace kernel {
// MI dispatch: nr in rax, up to 6 args in rdi/rsi/rdx/r10/r8/r9 (x86_64) or ebx/.../ebp (i686).
// Args are uintptr_t so 64-bit user pointers/sizes pass intact; the result is `long` so a 64-bit
// address (mmap) returns whole. `tf` is the opaque trap frame (execve/fork rewrite it). Defined in
// kernel/SyscallDispatch.cpp.
long kernelSyscall(long nr, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3,
		uintptr_t a4, uintptr_t a5, arch::TrapFrame* tf);
}

namespace arch {
// Install the trap handler that decodes registers and calls kernelSyscall.
void syscallInit();
// Boot sanity: issue one syscall through the real trap path ("syscall write OK").
void syscallSelfTest();
}
