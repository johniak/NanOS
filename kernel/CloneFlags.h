/*
 * CloneFlags.h — clone(2) flag constants + pure predicates (MI, arch-free, host-tested).
 *
 * clone()'s first argument packs the CLONE_* sharing flags (low byte may also be an exit
 * signal, e.g. SIGCHLD for a plain fork). The kernel only needs to ask two questions of it:
 * does the new task share the caller's address space (CLONE_VM), and is it a thread of the
 * same thread group (CLONE_THREAD)? Both are tiny pure predicates kept here, free of any
 * scheduler/arch state, so they are decided once and unit-tested on the host.
 *
 * Values are the Linux i386 ABI numbers (so musl/glibc clone wrappers pass them unchanged).
 */
#ifndef CLONEFLAGS_H_
#define CLONEFLAGS_H_

// The exit signal a plain fork-style clone carries in the low byte (clone(SIGCHLD,...)).
// A guarded macro (not an enum) so it never clashes with the host libc <signal.h> SIGCHLD
// or kernel/Signal.h, which use the same value/idiom.
#ifndef SIGCHLD
#define SIGCHLD 17
#endif

namespace kernel {

enum {
	CLONE_VM             = 0x00000100,   // share the address space
	CLONE_FS             = 0x00000200,   // share filesystem info (cwd/umask/root)
	CLONE_FILES          = 0x00000400,   // share the fd table
	CLONE_SIGHAND        = 0x00000800,   // share signal handlers
	CLONE_THREAD         = 0x00010000,   // same thread group (no new pid)
	CLONE_SYSVSEM        = 0x00040000,   // share System V semaphore undo state
	CLONE_SETTLS         = 0x00080000,   // set the child's TLS from the `tls` arg
	CLONE_PARENT_SETTID  = 0x00100000,   // write the child tid to *ptid in the parent
	CLONE_CHILD_CLEARTID = 0x00200000,   // zero + futex-wake *ctid on the child's exit
	CLONE_CHILD_SETTID   = 0x01000000,   // write the child tid to *ctid in the child
};

// Does this clone share the caller's address space (a thread, or a vfork-style share)?
inline bool cloneSharesAddressSpace(unsigned flags) { return (flags & CLONE_VM) != 0; }

// Is this clone a new thread of the SAME thread group (no new process/pid)?
inline bool cloneIsThread(unsigned flags) { return (flags & CLONE_THREAD) != 0; }

}

#endif /* CLONEFLAGS_H_ */
