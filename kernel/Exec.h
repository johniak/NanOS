/*
 * Exec.h
 *
 * Kernel-only glue to load and run a .nx program in ring 0: builds the export
 * table (the stable named API, backed by the Syscalls core), loads the image
 * via NxeLoader, and runs it with a setjmp/longjmp boundary so exit() returns
 * to the kernel. Single process, fixed load base 0x400000.
 */
#pragma once
#include "Vfs.h"

namespace kernel {
// Load <path> (.nx) from the VFS and run it; returns the program's exit code,
// or <0 on a load error. Top-level launch (called with the kernel directory
// active, e.g. from Kernel::start).
int execProgram(Vfs* vfs, const char* path);

// Spawn a child .nx synchronously from WITHIN a running ring-3 program (a syscall).
// Stages the image under the kernel directory, runs it to completion in its own
// address space, and returns its exit code (or <0 on error). argv must already be
// copied into kernel memory by the caller (the parent's space is swapped out while
// the child runs).
int spawnProgram(Vfs* vfs, const char* path, const char* const* argv, int argc);
}
