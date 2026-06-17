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
#include "FrameAllocator.h"
#include <arch/console.h>
#include <arch/bootinfo.h>
#include <arch/mmu.h>
#include <arch/cpu.h>             // arch::cpuInit / cpuEnableInterrupts / cpuHalt

namespace kernel { void irqSelfTest(); }   // arch/x86_64/cpu/irqtest64.cpp

namespace kernel {

// bootMemForEachUsable callback: open every usable physical RAM range in the frame pool.
// File-scope static (the contract's UsableRangeCb is a (void* ctx, base, len) function ptr,
// not a closure).
static void stageMarkUsable(void*, uint64_t base, uint64_t len) {
	kernel::g_frames.markRangeFree((uint32_t) base, (uint32_t) len);
}

void Kernel::start() {
	Console::clearScreen();

	// --- Plan 3: bring up the kernel's OWN 4-level paging BEFORE the banner, so the banner
	// proves the kernel survived the CR3 switch. Build the physical frame allocator from the
	// Multiboot map, then hand it to the arch MMU which builds the kernel PML4 (identity-maps
	// all RAM + enables NX), carves a real kernel heap off the top of RAM, and loads CR3 —
	// switching off the Plan-1 temporary 1 GiB map onto our own page tables.
	// mmuInitKernel ends with `sti`, but there is still no IDT at this point (Plan 4 installs the
	// real one below via cpuInit). Mask every PIC IRQ first so enabling interrupts during paging
	// bring-up can't vector a PIT/keyboard IRQ through the empty IDT and triple-fault. (GRUB
	// leaves the PIC unremapped, so IRQ0 would otherwise hit vector 0x08.) cpuInit() re-remaps the
	// PIC afterwards, and we then unmask exactly IRQ0+IRQ1 for the self-test.
	__asm__ __volatile__("outb %0, $0x21" :: "a"((uint8_t) 0xFF));
	__asm__ __volatile__("outb %0, $0xA1" :: "a"((uint8_t) 0xFF));

	uint32_t topOfRam = (uint32_t) arch::bootMemTop();
	kernel::g_frames.init(topOfRam);
	arch::bootMemForEachUsable(0, stageMarkUsable);
	arch::mmuInitKernel(kernel::g_frames, topOfRam);
	// CR3 now points at the kernel's own 4-level PML4 (NX enabled), not the Plan-1 temp map.

	Console::writeLine("");
	Console::writeLine("    NanOS x86_64  --  staged bring-up (Plan 3)");
	Console::writeLine("");
	Console::writeLine("[ OK ] long mode + MI kmain() reached");
	Console::writeLine("[ OK ] MI Console formatting over the x86_64 VGA sink");
	Console::writeLine("[ OK ] kernel PML4 active (4-level paging + NX)");

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

	// Exercise the REAL kernel byte heap that mmuInitKernel carved off the top of RAM (no
	// separate staging arena now): allocate, print the (64-bit-capable) pointer, free.
	void* p = malloc(128);
	Console::write("  heap: malloc(128) -> ");
	Console::writeHex((unsigned long) p);
	Console::writeLine("");
	free(p);

	// Proof the 64-bit hex path prints the upper 32 bits (the narrow Console LP64 fix).
	Console::write("  hex64 check: ");
	Console::writeHex((unsigned long) 0x123456789ABCUL);
	Console::writeLine("");

	// Plan 4: real GDT/IDT/TSS + PIC remap, then install the bring-up IRQ handlers and enable
	// interrupts. cpuInit()'s PIC remap leaves all lines unmasked; narrow that to exactly IRQ0
	// (PIT) + IRQ1 (keyboard) — master mask 0xFC (bits 0,1 clear), slave fully masked 0xFF — so
	// only the two self-test sources fire. After sti the PIT heartbeat ('.') and keyboard echo
	// are live.
	arch::cpuInit();
	__asm__ __volatile__("outb %0, $0x21" :: "a"((uint8_t) 0xFC));
	__asm__ __volatile__("outb %0, $0xA1" :: "a"((uint8_t) 0xFF));
	kernel::irqSelfTest();
	arch::cpuEnableInterrupts();   // sti
	Console::writeLine("[ OK ] interrupts live: PIT IRQ0 heartbeat + keyboard IRQ1 echo");

	Console::writeLine("");
	Console::writeLine("[ idle ] staged kernel parked (hlt loop)");

	for (;;)
		arch::cpuHalt();
}

}  // namespace kernel
