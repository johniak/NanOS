/*
 * Bkl.cpp — the single Big Kernel Lock instance + the C entry points the asm stubs call.
 *
 * bklEnter/bklExit are extern "C" so the SYSCALL/IRQ/ISR entry stubs (syscall_entry64.S,
 * irq64.S, isr64.S) can bracket the whole kernel episode — including the deferred-preemption
 * schedPreempt and signalDeliver that run after the C handler — with a matched pair. The
 * scheduler's context-switch handoff (release-before-switch / re-acquire-after) and the one-way
 * ring-3 entry archEnterUser call g_bkl directly / via bklExit. See kernel/Bkl.h for the model.
 */
#include "Bkl.h"

namespace kernel {
Bkl g_bkl;
}

extern "C" void bklEnter() { kernel::g_bkl.enter(); }
extern "C" void bklExit()  { kernel::g_bkl.exit(); }
