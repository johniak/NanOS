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
#include <arch/usermode.h>        // archLoadUser / archEnterUser (ring-3 launch)
#include "BlockDevice.h"
#include "DeviceManager.h"
#include "Vfs.h"
#include "Ext2Filesystem.h"
#include "Ext4Filesystem.h"
#include "Clock.h"                // wallClockSeconds/setBootEpoch — ext write path timestamps inodes
#include "NxeLoader.h"            // validate/relocate/bind the .nxe image
#include "NxFormat.h"             // NxHeader (v4 on x86_64) + nxaddr_t
#include "SyscallNr.h"            // SYS_* numbers (x86_64 ABI) for the staged dispatch
#include "Interrupt64.h"          // kernel::Registers (x86_64 trap frame the syscall stub builds)
#include "string.h"

// nxjmp64.S — minimal setjmp/longjmp so the user program's exit() unwinds back into the kernel
// (it switched to its own stack + CR3; longjmp restores this kernel frame). Only the pointer
// matters to the asm; NxJmp's size just needs to cover the 8 saved qwords it writes.
struct NxJmp { uint64_t v[8]; };
extern "C" long nx_setjmp(NxJmp*);
extern "C" void nx_longjmp(NxJmp*, long);
// syscall_entry64.S — the LSTAR target (SYSCALL fast-path entry).
extern "C" void syscall_entry();

// ---------------------------------------------------------------------------------------------
// Plan 6 (Tasks 11+13), STAGED ring-3 userland. The committed arch syscall path
// (arch/x86_64/cpu/syscall_x86_64.cpp's syscall_dispatch64) routes through the MI
// kernel::kernelSyscall + Syscalls core, which drag in the scheduler / ProcTable / net stack —
// none ported into this staged slice. So, exactly as the storage stack was wired into this
// staged kernel (instead of kernel/Kernel.cpp), we provide a MINIMAL inline syscall path here:
// our own syscall MSR init + per-CPU block + a tiny dispatch over the mounted Vfs serving the
// handful of syscalls the freestanding init uses (write/read/open/close/lseek/exit). syscall_
// x86_64.o is therefore NOT linked into the staged kernel; we own syscallInit/syscallSetKernel
// Stack here. The real kernel/Kernel.cpp + full Syscalls supersede this once the whole x86_64
// _all link exists (Plans 7+).
// ---------------------------------------------------------------------------------------------
namespace arch {

// Per-CPU block reached via %gs after swapgs (syscall_entry64.S reads [gs:0]=kernel stack top,
// [gs:8]=user rsp scratch). Same layout as syscall_x86_64.cpp's PerCpu — owned here because that
// translation unit is not linked into the staged kernel.
struct StagePerCpu { uint64_t kernelStackTop; uint64_t userRspScratch; };
static StagePerCpu g_stagePerCpu;
// usermode_x86_64.cpp's archEnterUser calls this (declared extern there) to publish the kernel
// stack top the syscall stub switches to.
void syscallSetKernelStack(uint64_t top) { g_stagePerCpu.kernelStackTop = top; }

static inline void stageWrmsr(uint32_t msr, uint64_t v) {
	__asm__ __volatile__("wrmsr" :: "c"(msr), "a"((uint32_t) v), "d"((uint32_t)(v >> 32)));
}
static inline uint64_t stageRdmsr(uint32_t msr) {
	uint32_t lo, hi;
	__asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((uint64_t) hi << 32) | lo;
}

// Program EFER.SCE / STAR / LSTAR / FMASK + KERNEL_GS_BASE. Mirrors arch::syscallInit in
// syscall_x86_64.cpp exactly (same STAR, the committed SYSRET-compatible GDT layout).
void syscallInit() {
	stageWrmsr(0xC0000080, stageRdmsr(0xC0000080) | 1);                        // EFER.SCE
	stageWrmsr(0xC0000081, ((uint64_t) 0x08 << 32) | ((uint64_t) 0x18 << 48)); // STAR
	stageWrmsr(0xC0000082, (uint64_t) &syscall_entry);                         // LSTAR
	stageWrmsr(0xC0000084, (1u << 9) | (1u << 10));                            // FMASK: IF | DF
	stageWrmsr(0xC0000102, (uint64_t) &g_stagePerCpu);                         // IA32_KERNEL_GS_BASE
}

}  // namespace arch

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

// ---- Staged ring-3 launch (Plan 6 Tasks 11+13) ----------------------------------------------
// The staging/user window: VA_USER_BASE (0x800000) .. VA_USER_END (0x1000000). The .nxe is read
// here under the kernel directory (where 0x800000 is identity-mapped), validated/relocated by
// NxeLoader, then archLoadUser copies it into the process's private USER frames at the same VA.
static const unsigned STAGE_BASE = 0x800000;
static const unsigned STAGE_CAP  = 0x800000;   // 8 MiB — the whole user window

// Minimal staged fd table for init.nxe. 0/1/2 are the console; >= 3 are files open on the Vfs.
struct StageFd { bool used; String path; unsigned off; };
static const int STAGE_MAXFD = 16;
static StageFd g_sfd[STAGE_MAXFD];
static Vfs*    g_userVfs = 0;
static NxJmp   g_exitJmp;
static int     g_exitCode = 0;

static void stageFdReset() {
	for (int i = 0; i < STAGE_MAXFD; i++) { g_sfd[i].used = false; g_sfd[i].off = 0; }
}

// The tiny syscall core for the staged userland (AMD64 SysV: nr=rax, args rdi/rsi/rdx). Serves
// only what the freestanding init.nxe issues; everything else is -ENOSYS. exit() longjmps back
// to launchInit instead of returning. Negative returns use the x86_64 errno values.
static long stageSyscall(long nr, long a0, long a1, long a2) {
	switch (nr) {
	case SYS_write:                                   // write(fd, buf, len) — console only
		if (a0 == 1 || a0 == 2) {
			const char* b = (const char*) a1;
			for (long i = 0; i < a2; i++) Console::write(b[i]);
			return a2;
		}
		return -9;                                    // -EBADF
	case SYS_read: {                                  // read(fd, buf, len)
		int fd = (int) a0;
		if (fd < 3 || fd >= STAGE_MAXFD || !g_sfd[fd].used) return -9;   // -EBADF
		int n = g_userVfs->read(g_sfd[fd].path, (unsigned) a2, g_sfd[fd].off, (void*) a1);
		if (n > 0) g_sfd[fd].off += (unsigned) n;
		return n;
	}
	case SYS_open: {                                  // open(path, flags)
		String p = String((char*) a0);
		FileStat st;
		if (g_userVfs->stat(p, st) < 0) return -2;    // -ENOENT
		for (int fd = 3; fd < STAGE_MAXFD; fd++)
			if (!g_sfd[fd].used) { g_sfd[fd].used = true; g_sfd[fd].path = p; g_sfd[fd].off = 0; return fd; }
		return -24;                                   // -EMFILE
	}
	case SYS_close:                                   // close(fd)
		if (a0 >= 3 && a0 < STAGE_MAXFD) g_sfd[a0].used = false;
		return 0;
	case SYS_lseek:                                   // lseek(fd, off, SEEK_SET)
		if (a0 >= 3 && a0 < STAGE_MAXFD && g_sfd[a0].used) { g_sfd[a0].off = (unsigned) a1; return a1; }
		return -9;                                    // -EBADF
	case SYS_exit:                                    // exit(code) — back to the kernel
		g_exitCode = (int) a0;
		nx_longjmp(&g_exitJmp, 1);                    // unwinds to launchInit's setjmp; no return
		return 0;                                     // unreachable
	default:
		return -38;                                   // -ENOSYS
	}
}

// Load /disks/main/nanos/core/init.nxe and run it in ring 3. Returns to the kernel after the
// program calls exit() (via the nx_longjmp in stageSyscall). On any load failure it prints a
// diagnostic and returns without entering ring 3.
static void launchInit(Vfs* vfs) {
	const char* path = "/disks/main/nanos/core/init.nxe";
	g_userVfs = vfs;
	FileStat st;
	if (vfs->stat(String((char*) path), st) < 0) {
		Console::writeLine("[ !! ] init.nxe not found at /disks/main/nanos/core/init.nxe");
		return;
	}
	if (st.size > STAGE_CAP) {
		Console::writeLine("[ !! ] init.nxe too large to stage");
		return;
	}
	char* image = (char*) (uintptr_t) STAGE_BASE;
	if (vfs->read(String((char*) path), st.size, 0, image) < 0) {
		Console::writeLine("[ !! ] init.nxe read failed");
		return;
	}
	nxaddr_t entry = 0;
	int rc = NxeLoader::loadImage(image, STAGE_CAP, 0, 0, &entry);   // EXE: delta 0, no imports
	if (rc < 0) {
		Console::write("[ !! ] init.nxe NxeLoader rejected (rc=");
		Console::write(rc);
		Console::writeLine(")");
		return;
	}
	NxHeader* h = (NxHeader*) image;
	arch::AddressSpace* space = arch::mmuCreateAddressSpace();
	const char* argv[] = { "init.nxe", 0 };
	const char* envp[] = { 0 };
	uintptr_t esp = arch::archLoadUser(space, (uintptr_t) h->loadBase, (uintptr_t) h->bssEnd,
			argv, 1, envp, 0);
	stageFdReset();

	Console::writeLine("");
	Console::writeLine("[ .. ] entering ring 3 -> init.nxe (SYSCALL/SYSRET)");
	Console::writeLine("");

	// Mask every PIC IRQ for the ring-3 run: the PIT/keyboard heartbeat handlers call the
	// non-reentrant MI Console, which would scramble init's output. (The syscall body itself runs
	// with IF=0 from FMASK.) The idle loop below re-arms IRQ0+IRQ1 and re-enables interrupts.
	__asm__ __volatile__("outb %0, $0x21" :: "a"((uint8_t) 0xFF));
	__asm__ __volatile__("outb %0, $0xA1" :: "a"((uint8_t) 0xFF));

	if (nx_setjmp(&g_exitJmp) == 0)
		arch::archEnterUser(entry, esp, space);   // ring 3; returns here via nx_longjmp on exit()

	// Back from init's exit(): we longjmp'd out while still on the user CR3 — restore the kernel
	// address space before freeing the process's page tables.
	arch::mmuLoadDirPhys(arch::mmuKernelDirPhys());
	Console::write("[ OK ] init.nxe exited (rc=");
	Console::write(g_exitCode);
	Console::writeLine(")");
	arch::mmuFreeAddressSpace(space);
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

	// --- Plan 6 (Tasks 11+13): program the SYSCALL MSRs and run the 64-bit init.nxe in ring 3.
	// init writes a banner, cats grub.cfg through write()/open()/read() syscalls, then exit()s
	// back here. Interrupts stay disabled across this (launchInit masks the PIC for the run); the
	// idle loop below restores the heartbeat.
	Console::writeLine("");
	arch::syscallInit();
	launchInit(vfs);

	Console::writeLine("");
	Console::writeLine("[ idle ] staged kernel parked (hlt loop)");

	// Re-arm IRQ0 (PIT) + IRQ1 (keyboard) — launchInit masked the whole PIC for the ring-3 run —
	// then re-enable interrupts for the idle heartbeat.
	__asm__ __volatile__("outb %0, $0x21" :: "a"((uint8_t) 0xFC));
	__asm__ __volatile__("outb %0, $0xA1" :: "a"((uint8_t) 0xFF));
	arch::cpuEnableInterrupts();   // restore the live PIT/keyboard heartbeat for the idle loop
	for (;;)
		arch::cpuHalt();
}

}  // namespace kernel

// SYSCALL fast-path C entry (called by syscall_entry64.S with a kernel::Registers* the stub
// built). The staged replacement for syscall_x86_64.cpp's syscall_dispatch64: decode the AMD64
// SysV registers and forward to the minimal staged dispatch. Interrupts stay OFF (IF cleared by
// FMASK, PIC masked during the ring-3 run) so the polling ext/ATA path and the non-reentrant
// Console are never interrupted. exit() does not return here (it longjmps out of stageSyscall).
extern "C" void syscall_dispatch64(kernel::Registers* r) {
	r->rax = (uint64_t) kernel::stageSyscall((long) r->rax, (long) r->rdi, (long) r->rsi,
			(long) r->rdx);
}
