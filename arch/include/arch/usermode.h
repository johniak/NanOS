/*
 * arch/usermode.h — MI/MD contract for running a user program.
 *
 * The kernel loads a program image (MI) and hands the entry point to the arch,
 * which runs it and returns its exit code when the program exits via a syscall.
 * The ring transition, TSS, and (later) per-process address-space switch are all
 * arch-specific and hidden here.
 */
#pragma once
#include <stdint.h>

namespace arch {

struct AddressSpace;   // opaque per-process space (see <arch/mmu.h>); 0 = run in ring 0

// Run the program at `entry` with stack top `userStackTop`. Returns the exit
// code once the program calls exit() (which traps and longjmps back here).
// space == 0 runs in ring 0; otherwise ring 3 in that address space.
int enterUser(uint32_t entry, uint32_t userStackTop, AddressSpace* space = 0);

// Load a staged program image (already at its load base, bss zeroed) into a
// fresh per-process address space, place an argv image on its stack, and run it
// in ring 3. Returns the exit code.
int execUserImage(uint32_t entry, uint32_t loadBase, uint32_t bssEnd,
                  const char* const* argv, int argc);

// Like execUserImage, but RE-ENTRANT: called from a syscall while a parent program
// is already running in ring 3 (a spawn). Saves/restores the single ring-3 return
// context and switches the CPU to a deeper kernel stack so the child's traps don't
// clobber the parent's in-flight frames. The caller (MI spawn glue) is responsible
// for staging the image under the kernel directory and restoring its own CR3.
int spawnUserImage(uint32_t entry, uint32_t loadBase, uint32_t bssEnd,
                   const char* const* argv, int argc);

// Called by the syscall trap when the running program has exited: returns
// control to the kernel (longjmp back into enterUser).
void userExit();

}
