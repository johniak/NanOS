/*
 * SyscallDispatch.h
 *
 * Machine-independent syscall layer: installs the Syscalls core over the VFS and
 * exposes kernel::kernelSyscall (the syscall-number switch) that the arch trap
 * handler calls. The register decode / int 0x80 wiring lives behind
 * <arch/syscall.h> in the arch layer.
 */
#pragma once
#include "Vfs.h"
#include "Syscall.h"

namespace kernel {
struct Task;   // scheduler task (Scheduler.h) — only stored/forwarded across this boundary
void installSyscalls(Vfs* vfs);
Syscalls* kernelSyscalls();   // the installed Syscalls instance (0 if not yet installed)

// Wake up to `n` threads parked in futex(FUTEX_WAIT) on `uaddr` within address space `space`.
// A kernel-callable handle on the futex table for the CLONE_CHILD_CLEARTID handshake (a thread
// exit zeroes the tid word and wakes a joiner). Thin wrapper over the FUTEX_WAKE path.
void futexWakeAddr(const void* space, void* uaddr, int n);

// Evict every futex waiter owned by task `t` (within address space `space`) from the table.
// Called before a thread's kernel stack is reaped (execve sibling-teardown / exit_group) so the
// futex buckets never keep a pointer into the freed kstack the waiter node lived on.
void futexRemoveTask(const void* space, Task* t);
}
