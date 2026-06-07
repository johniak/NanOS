/*
 * arch/irq.h — MI/MD contract for interrupt handler registration.
 *
 * The trap frame is opaque to MI code; each arch defines its own register
 * layout (x86: kernel::Registers). MI code that needs to react to an interrupt
 * registers an IrqHandler and treats the frame as opaque; code that needs the
 * actual register contents is inherently arch-specific.
 */
#pragma once

namespace arch {

struct TrapFrame;                            // opaque; arch-defined
typedef void (*IrqHandler)(TrapFrame*);

// Arch-neutral device interrupt lines.
enum { IRQ_TIMER = 0, IRQ_KEYBOARD = 1 };

void registerIrqHandler(unsigned irq, IrqHandler h);     // device IRQ line
void registerTrapHandler(unsigned vector, IrqHandler h); // raw CPU vector

}
