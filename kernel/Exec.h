/*
 * Exec.h
 *
 * Kernel-only glue for the process image lifecycle (Stage 4 trap-frame model):
 *   - execProgram: PID 1's first run — load a .nxe into a fresh address space and
 *     enter ring 3 (does not return on success; the process runs until it exits).
 *   - execve:      replace the calling process's image in place (rewrites the live
 *     syscall trap frame so the iret lands in the new program).
 *   - procExit:    tear down the current process's address space and deschedule it
 *     (called from the syscall trap after SYS_exit; does not return).
 * Programs talk to the kernel via int 0x80; the IAT import path is gone.
 */
#pragma once
#include "Vfs.h"

namespace arch { struct TrapFrame; }   // opaque syscall trap frame (<arch/usermode.h>)

namespace kernel {

// PID 1 launch: load <path> (.nxe) into a new address space and enter ring 3. Called
// from the init task body with the kernel directory active. Returns <0 on a load
// error; does NOT return on success (the process runs until it exits).
int execProgram(Vfs* vfs, const char* path);

// execve(2): replace the current process's image with <path> (.nxe). `argv`/`argc` and
// `envp`/`envc` are already copied into kernel memory. On success rewrites *tf so the
// syscall's iret enters the new program (no meaningful return); returns <0 on a load error.
int execve(Vfs* vfs, const char* path, const char* const* argv, int argc,
		const char* const* envp, int envc, arch::TrapFrame* tf);

// fork(2): create a child process — eager copy of the current process's address
// space + fd table — that resumes from the same trap frame *tf with a return value
// of 0. Returns the child's pid to the parent, or <0 on failure (-EAGAIN).
int forkProcess(arch::TrapFrame* tf);

// waitpid(2): wait on a child of the current process (matching `wantPid`, or any child
// when wantPid <= 0); write its W*-encoded status to *statusOut and return its pid.
// `options` are the Linux bits: WNOHANG (1) returns 0 rather than blocking, WUNTRACED
// (2) also reports a child that has just stopped. Returns -ECHILD when there is no child.
int waitProcess(int wantPid, int* statusOut, int options);

// SYS_exit handler tail: free the current process's address space, mark its task a
// zombie, and schedule away. Does NOT return.
void procExit();

// Kill the current (ring-3) process with a fatal signal — the CPU-exception backstop: a user
// program that faults (bad pointer, #GP, ...) is terminated like a SIGSEGV instead of taking the
// whole system down. Mirrors a fatal signal: the parent gets SIGCHLD/WIFSIGNALED. Does NOT return.
void killCurrentProcess(int sig);

}
