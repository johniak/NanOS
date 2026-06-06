/*
 * Exec.h
 *
 * Kernel-only glue to load and run a .nx program in ring 0: builds the export
 * table (the stable named API, backed by the Syscalls core), loads the image
 * via ExeLoader, and runs it with a setjmp/longjmp boundary so exit() returns
 * to the kernel. Single process, fixed load base 0x400000.
 */
#pragma once
#include "Vfs.h"

namespace kernel {
// Load <path> (.nx) from the VFS and run it; returns the program's exit code,
// or <0 on a load error.
int execProgram(Vfs* vfs, const char* path);
}
