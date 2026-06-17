/*
 * arch/usermode.h — MI/MD contract for running ring-3 code.
 *
 * Stage 4 model: a process is a scheduler task that runs until it exits. There is no
 * synchronous "call the program and get its exit code back" — the kernel sets up a
 * user address space, loads an image into it, and enters ring 3 via `iret`
 * (archEnterUser, which does not return). Syscalls return by `iret`-ing the trap
 * frame; exit deschedules; fork copies the frame (see <arch/sched.h> / Process).
 */
#pragma once
#include <stdint.h>

namespace arch {

struct AddressSpace;   // opaque per-process space (<arch/mmu.h>)

// Map a staged program image (already at its load base in the kernel staging window,
// bss zeroed) plus a heap and a stack into `space`, writing the SysV argv+envp image onto
// the stack. Returns the initial user esp (pointing at argc).
// Address-typed parameters use uintptr_t so the SAME contract serves i686 (32-bit) and
// x86_64 (64-bit): uintptr_t == uint32_t on i686-elf (verified), so usermode_x86.cpp keeps
// its uint32_t signatures unchanged, while usermode_x86_64.cpp gets 64-bit addresses.
uintptr_t archLoadUser(AddressSpace* space, uintptr_t loadBase, uintptr_t bssEnd,
                       const char* const* argv, int argc,
                       const char* const* envp, int envc);

// Enter ring 3 at `entry` with stack `userEsp` in `space`. Switches CR3 and `iret`s
// down to CPL 3. DOES NOT RETURN (the process runs until it exits).
void archEnterUser(uintptr_t entry, uintptr_t userEsp, AddressSpace* space);

// Map a relocated shared-library image (`bytes` of code/data, already fixed up for
// `base` by the loader) into `space` at `base`: fresh private USER|RW frames, the bytes
// copied in, the tail of the last page and any bss zeroed. Called under the kernel
// directory (frame allocation touches RAM by identity), like archLoadUser.
void archLoadModule(AddressSpace* space, uintptr_t base, const void* img, uintptr_t bytes);

struct TrapFrame;   // opaque syscall/IRQ trap frame (the x86 Registers); see <arch/irq.h>

// Rewrite a syscall trap frame so its `iret` re-enters ring 3 at `entry` with stack
// `userEsp` (used by execve to morph the calling process into a freshly loaded image).
void archFrameToUser(TrapFrame* tf, uintptr_t entry, uintptr_t userEsp);

// How an interrupted syscall is resumed after a handler runs, baked into the saved frame:
enum {
	SIG_FRAME_KEEP    = 0,   // not a restartable syscall: preserve eip + the result in eax
	SIG_FRAME_RESTART = 1,   // SA_RESTART: rewind eip to the int 0x80 and restore eax = nr
	SIG_FRAME_EINTR   = 2,   // no SA_RESTART: leave eip, set eax = -EINTR
};

// Push a signal-handler frame onto the user stack and retarget `tf` so the return-to-user
// `iret` enters `handler(sig)` in ring 3. On the handler's `ret` it lands in the libc
// trampoline `restorer`, which invokes sigreturn. `oldMask` is the signal mask restored at
// sigreturn. `restartAction` (SIG_FRAME_*) decides what the resumed context does about an
// interrupted syscall; `origEax` is the syscall number to restore when restarting.
void archPushSignalFrame(TrapFrame* tf, uintptr_t handler, uintptr_t restorer,
                         int sig, uint64_t oldMask, uintptr_t origEax, int restartAction);

// SYS_sigreturn: restore `tf` from the user-stack signal frame; writes the full 64-bit
// mask to be restored to *oldMaskOut and returns the interrupted code's saved eax.
int archSigreturn(TrapFrame* tf, uint64_t* oldMaskOut);

// The current syscall result sitting in the trap frame (eax), as a signed int.
int archSyscallResult(TrapFrame* tf);

// Rewind a trap frame so its iret re-executes the `int 0x80` (eip -= 2) with eax = the
// original syscall number — i.e. restart the interrupted syscall in place (no handler).
void archRestartSyscall(TrapFrame* tf, uintptr_t origEax);

// Set the user thread pointer for the current/next ring-3 run. On x86_64 this programs the
// IA32_FS_BASE MSR so picolibc's %fs-relative TLS (errno/stdio, decision #2) resolves; on
// i686 TLS lives in a GDT %gs entry, so this is a no-op stub.
void archSetUserFsBase(uint64_t base);

}
