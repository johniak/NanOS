// arch/x86_64/boot/KernelStage64.cpp — Plan-2 STAGED minimal kernel::Kernel::start for the
// x86_64 bring-up. Deliberately NOT the full kernel/Kernel.cpp (which drags in the storage
// stack, scheduler, syscalls and every arch contract not yet ported). This staged start
// proves the long-mode kernel can: reach MI kmain(), drive the real MI Console formatting
// layer over the x86_64 VGA sink, read the Multiboot memory map parsed in 64-bit
// (arch::bootMemTop / bootFramebuffer), exercise the MI byte heap (memory_manager) under
// LP64, then idle. Linked ONLY into the staged image; the real kernel/Kernel.cpp supersedes
// it once Plans 3-5 land the missing arch layers (paging/interrupts/storage). The idle
// hlt-loop uses inline asm directly — this is MD-located code, exempt from the MI guard.
#include "Kernel.h"
#include "Console.h"
#include "memory_manager.h"
#include <arch/console.h>
#include <arch/bootinfo.h>

namespace kernel {

// A small scratch heap inside the loader's 1 GiB identity map (the kernel lives at 1 MiB and
// is tiny). 16 MiB base, 1 MiB arena — comfortably within QEMU's 512 MiB and the identity map.
static const unsigned long STAGE_HEAP_BASE = 0x01000000UL;   // 16 MiB
static const unsigned      STAGE_HEAP_SIZE = 0x00100000U;    // 1 MiB

void Kernel::start() {
	Console::clearScreen();
	Console::writeLine("");
	Console::writeLine("    NanOS x86_64  --  staged bring-up (Plan 2)");
	Console::writeLine("");
	Console::writeLine("[ OK ] long mode + MI kmain() reached");
	Console::writeLine("[ OK ] MI Console formatting over the x86_64 VGA sink");

	// Memory map parsed by bootinfo_x86_64 from the Multiboot1 info (64-bit walk).
	unsigned long top = (unsigned long) arch::bootMemTop();
	Console::write("  RAM top: ");
	Console::writeHex(top);
	Console::write("  (");
	Console::write((int) (top / (1024UL * 1024UL)));
	Console::writeLine(" MiB usable)");

	// Framebuffer (none under the Plan-1 multiboot header, which requests no video mode).
	const arch::BootFramebuffer* fb = arch::bootFramebuffer();
	if (fb) {
		Console::write("  Framebuffer @ ");
		Console::writeHex((unsigned long) fb->addr);
		Console::write("  ");
		Console::write((int) fb->width);
		Console::write("x");
		Console::write((int) fb->height);
		Console::write("x");
		Console::writeLine((int) fb->bpp);
	} else {
		Console::writeLine("  Framebuffer: none (VGA text mode)");
	}

	// Exercise the MI byte heap under LP64 (8-byte pointers): lay out the arena, allocate,
	// print the (64-bit-capable) pointer, free.
	heapInit((void*) STAGE_HEAP_BASE, STAGE_HEAP_SIZE);
	void* p = malloc(128);
	Console::write("  heap: malloc(128) -> ");
	Console::writeHex((unsigned long) p);
	Console::writeLine("");
	free(p);

	// Proof the 64-bit hex path prints the upper 32 bits (the narrow Console LP64 fix).
	Console::write("  hex64 check: ");
	Console::writeHex((unsigned long) 0x123456789ABCUL);
	Console::writeLine("");

	Console::writeLine("");
	Console::writeLine("[ idle ] staged kernel parked (hlt loop)");

	for (;;)
		__asm__ __volatile__("hlt");
}

}  // namespace kernel
