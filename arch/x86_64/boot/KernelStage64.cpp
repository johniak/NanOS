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
#include <arch/block.h>           // arch::bootDisk() — the x86_64 ATA boot disk factory
#include "BlockDevice.h"
#include "DeviceManager.h"
#include "Vfs.h"
#include "Ext2Filesystem.h"
#include "Ext4Filesystem.h"
#include "Clock.h"                // wallClockSeconds/setBootEpoch — ext write path timestamps inodes
#include "string.h"

namespace kernel { void irqSelfTest(); }   // arch/x86_64/cpu/irqtest64.cpp

namespace kernel {

// Staged Clock implementation. The real impl lives in kernel/Syscall.cpp (NOT linked into the
// staged slice — it drags in the whole syscall/scheduler/net world). The ext write path only
// needs these three symbols to timestamp inodes; a fixed boot epoch is fine for the self-test
// (Clock.h: a 0/constant time is harmless for on-disk timestamps). Superseded by the real
// kernel/Kernel.cpp + Syscall.cpp once the full x86_64 _all link exists.
static unsigned g_stageEpoch = 0;
void     setBootEpoch(unsigned s) { g_stageEpoch = s; }
unsigned bootEpochSeconds()       { return g_stageEpoch; }
unsigned wallClockSeconds()       { return g_stageEpoch; }

// Discover the first partition's start LBA from the MBR (mirrors kernel/Kernel.cpp). Reads
// sector 0; if it carries the 0x55AA signature, returns the start LBA of the first non-empty
// entry. Falls back to 2048 (the image's GRUB layout) when there is no valid MBR.
static unsigned firstPartitionLba(BlockDevice* dev) {
	unsigned char mbr[512];
	if (dev->readSectors(0, 1, mbr) != 0)
		return 2048;
	if (mbr[510] != 0x55 || mbr[511] != 0xAA)
		return 2048;
	for (int i = 0; i < 4; i++) {
		unsigned char* e = mbr + 0x1BE + i * 16;
		unsigned type = e[4];
		unsigned start = e[8] | (e[9] << 8) | (e[10] << 16) | ((unsigned) e[11] << 24);
		if (type != 0 && start != 0)
			return start;
	}
	return 2048;
}

// One-shot read-write self-test of the persistent disk (mirrors extRwSelftest in
// kernel/Kernel.cpp): (re)write a marker file, read it back, and report whether last boot's
// marker survived — proving VFS -> ext write + JBD2 -> ATA write -> physical disk. Mutates only
// /disks/main/nanos/rwtest64; e2fsck-clean afterwards.
static void stageExtRwSelftest(Vfs* vfs) {
	const char* path = "/disks/main/nanos/rwtest64";
	const char* marker = "NANOS-RW64-OK";
	unsigned mlen = (unsigned) strlen(marker);
	char prev[32];
	Console::writeLine("  rw: A read-prev");
	int pn = vfs->read(String(path), 31, 0, prev);
	bool persisted = pn == (int) mlen;
	for (unsigned i = 0; persisted && i < mlen; i++)
		if (prev[i] != marker[i]) persisted = false;

	Console::write("  rw: B create (prev pn="); Console::write(pn); Console::writeLine(")");
	int cr = vfs->create(String(path), 0644);
	Console::write("  rw: C write (cr="); Console::write(cr); Console::writeLine(")");
	int wr = (cr == 0) ? vfs->write(String(path), mlen, 0, marker) : cr;
	Console::write("  rw: D read-back (wr="); Console::write(wr); Console::writeLine(")");
	char back[32];
	int rn = vfs->read(String(path), 31, 0, back);
	Console::write("  rw: E done (rn="); Console::write(rn); Console::writeLine(")");
	bool ok = wr == (int) mlen && rn == (int) mlen;
	for (unsigned i = 0; ok && i < mlen; i++)
		if (back[i] != marker[i]) ok = false;

	Console::write("[ ");
	Console::write(ok ? "OK" : "!!");
	Console::write(" ] EXT-RW selftest: ");
	Console::write(ok ? "write+read OK" : "FAIL");
	Console::writeLine(persisted ? " (marker persisted from last boot)"
	                              : " (first write to this image)");
}

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

	// --- Plan 5: bring up the MI storage stack on the real x86_64 ATA disk. Register the arch
	// boot disk, mount its ext partition under /disks/main (auto-detect ext2/ext4), read a known
	// file (ext READ over ATA64), then run the ext write self-test (ext WRITE + JBD2 -> ATA write
	// -> disk). This proves ATA64 + ext read/write on x86_64 within the staged model.
	//
	// Mask interrupts for the duration: the ext/ATA path is pure polling (needs no IRQs), and the
	// PIT/keyboard heartbeat handlers above call the NON-reentrant MI Console. The storage section
	// does slow ATA-PIO + JBD2 disk I/O, so a heartbeat firing mid-write would re-enter Console and
	// corrupt the cursor/scroll state, scrambling these result lines. cpuEnableInterrupts() restores
	// the live heartbeat before the idle loop.
	arch::cpuDisableInterrupts();
	Console::writeLine("");
	BlockDevice* hd0 = arch::bootDisk();
	DeviceManager::registerDevice(hd0);
	Vfs* vfs = new Vfs();
	vfs->registerType(new Ext4FileSystemType());
	vfs->registerType(new Ext2FileSystemType());

	unsigned lba = firstPartitionLba(hd0);
	Console::write("  MBR: first partition LBA = ");
	Console::write((int) lba);
	Console::writeLine("");

	int mr = vfs->mount("/disks/main", "auto", hd0, lba);
	Console::write(mr == 0 ? "[ OK ]" : "[ !! ]");
	Console::writeLine(" mounted ext filesystem at /disks/main");

	// ext READ proof: read a file known to exist on the image (the GRUB config) and show its size
	// and first bytes — proves the ext driver resolves blocks over real ATA64 PIO reads.
	{
		const char* rp = "/disks/main/boot/grub/grub.cfg";
		char rb[64];
		int n = vfs->read(String(rp), sizeof(rb) - 1, 0, rb);
		Console::write(n > 0 ? "[ OK ]" : "[ !! ]");
		Console::write(" read ");
		Console::write(rp);
		Console::write(" -> ");
		Console::write(n);
		Console::writeLine(" bytes");
		if (n > 0) {
			rb[n < (int) sizeof(rb) - 1 ? n : (int) sizeof(rb) - 1] = 0;
			Console::write("       first bytes: ");
			for (int i = 0; i < n && i < 24; i++)
				Console::write(rb[i] == '\n' ? ' ' : rb[i]);
			Console::writeLine("");
		}
	}

	// ext WRITE proof (+ persistence across reboot via JBD2).
	stageExtRwSelftest(vfs);

	Console::writeLine("");
	Console::writeLine("[ idle ] staged kernel parked (hlt loop)");

	arch::cpuEnableInterrupts();   // restore the live PIT/keyboard heartbeat for the idle loop
	for (;;)
		arch::cpuHalt();
}

}  // namespace kernel
