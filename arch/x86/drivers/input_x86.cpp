/*
 * input_x86.cpp — x86 implementation of <arch/input.h>.
 *
 * Blocks until the keyboard line discipline (filled by the IRQ1 handler in
 * Keyboard.cpp) has a committed line, then copies it out. Blocking is sti + hlt:
 * the CPU sleeps until the next interrupt (the keyboard IRQ fills the buffer).
 */
#include <arch/input.h>
#include <arch/cpu.h>
#include "LineDiscipline.h"

namespace kernel { extern LineDiscipline g_lineDiscipline; }  // defined in Keyboard.cpp

namespace arch {

int inputReadLine(char* buf, unsigned n) {
	while (!kernel::g_lineDiscipline.lineReady()) {
		arch::cpuEnableInterrupts();   // ensure IRQ1 can fire
		arch::cpuHalt();               // sleep until the next interrupt
	}
	return kernel::g_lineDiscipline.takeLine(buf, (int) n);
}

}  // namespace arch
