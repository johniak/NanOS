/*
 * bringup_stubs64.cpp — Plan-2 staging shims for the few <arch/cpu.h> symbols the MI byte
 * heap (mm/memory_manager.cpp heapPanic) references at link time but that the full arch CPU
 * layer does not yet provide. Only the two actually referenced are defined; the real, full
 * arch/x86_64/cpu/cpu_x86_64.cpp (Plan 4) supersedes this file, which is linked ONLY into the
 * staged bring-up image, never the full kernel — so there is no duplicate-symbol clash later.
 */
#include <arch/cpu.h>

namespace arch {

void cpuDisableInterrupts() {
	__asm__ __volatile__("cli");
}

void cpuHalt() {
	__asm__ __volatile__("hlt");
}

}  // namespace arch
