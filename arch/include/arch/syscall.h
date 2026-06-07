/*
 * arch/syscall.h — MI/MD contract for the syscall boundary.
 *
 * The arch decodes its trap (x86: int 0x80, nr in eax, args in ebx/ecx/edx) and
 * calls the MI dispatcher kernel::kernelSyscall; the MI side maps the number to
 * the Syscalls core. The numbers themselves (our chosen Linux i386 ABI) live in
 * kernel/Syscall.h and are MI.
 */
#pragma once

namespace arch { struct TrapFrame; }   // opaque trap frame (passed through for execve/fork)

namespace kernel {
// MI dispatch: map a syscall number + up to 3 args to a result (negative errno
// on failure). `tf` is the opaque trap frame the arch is returning through — needed
// by syscalls that rewrite the caller's frame (execve) or fork it. Defined in
// kernel/SyscallDispatch.cpp.
int kernelSyscall(int nr, unsigned a0, unsigned a1, unsigned a2, arch::TrapFrame* tf);
}

namespace arch {
// Install the trap handler that decodes registers and calls kernelSyscall.
void syscallInit();
// Boot sanity: issue one syscall through the real trap path ("syscall write OK").
void syscallSelfTest();
}
