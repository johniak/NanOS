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
// bss zeroed) plus a heap and a stack into `space`, writing the SysV argv image onto
// the stack. Returns the initial user esp (pointing at argc).
uint32_t archLoadUser(AddressSpace* space, uint32_t loadBase, uint32_t bssEnd,
                      const char* const* argv, int argc);

// Enter ring 3 at `entry` with stack `userEsp` in `space`. Switches CR3 and `iret`s
// down to CPL 3. DOES NOT RETURN (the process runs until it exits).
void archEnterUser(uint32_t entry, uint32_t userEsp, AddressSpace* space);

struct TrapFrame;   // opaque syscall/IRQ trap frame (the x86 Registers); see <arch/irq.h>

// Rewrite a syscall trap frame so its `iret` re-enters ring 3 at `entry` with stack
// `userEsp` (used by execve to morph the calling process into a freshly loaded image).
void archFrameToUser(TrapFrame* tf, uint32_t entry, uint32_t userEsp);

}
