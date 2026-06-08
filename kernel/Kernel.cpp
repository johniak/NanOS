#include <stdint.h>
#include "Kernel.h"
#include "Console.h"
#include "BlockDevice.h"
#include "DeviceManager.h"
#include "Vfs.h"
#include "Ext2Filesystem.h"
#include "Ext4Filesystem.h"
#include "SynthFs.h"
#include "Framebuffer.h"
#include "Font8x16.h"
#include "Scheduler.h"
#include <arch/sched.h>
#include "Syscall.h"
#include "SyscallDispatch.h"
#include "Process.h"
#include "Exec.h"
#include "List.h"
#include "String.h"
#include <arch/bootinfo.h>
#include <arch/mmu.h>
#include <arch/cpu.h>
#include <arch/syscall.h>
#include <arch/block.h>
#include "FrameAllocator.h"
char buf[1024];

namespace kernel {

// Mark a usable physical range free in the frame allocator (arch reports only
// usable ranges via <arch/bootinfo.h>).
static void markFree(void* fa, uint64_t base, uint64_t len) {
	((FrameAllocator*) fa)->markRangeFree((uint32_t) base, (uint32_t) len);
}

// Mount a physical volume at /disks/<name> and register a marker under the synthetic
// /disks so it shows up in readdir. Single helper = one source of truth.
static void mountVolume(Vfs* vfs, SynthFs* root, const char* name, BlockDevice* dev,
		unsigned lba) {
	char mp[80];
	const char* pre = "/disks/";
	int i = 0;
	for (; pre[i]; i++)
		mp[i] = pre[i];
	for (int j = 0; name[j] && i < 79; j++, i++)
		mp[i] = name[j];
	mp[i] = 0;
	vfs->mount(String(mp), "auto", dev, lba);
	root->addVolume(name);
}

// Scheduler task bodies. Task 1 (init/nsh) enters ring 3 via execProgram; task 2 is
// a background kernel thread (demonstrates that several tasks coexist) — it sleeps and
// counts, so cat /proc/uptime advancing while the shell is idle proves the scheduler
// keeps running.
static Vfs* g_vfs = 0;
static volatile unsigned g_bgwork = 0;

static void initTaskBody() {
	// Enters ring 3 and does not return on success; only reached if the load fails.
	int rc = execProgram(g_vfs, "/disks/main/nanos/core/init.nxe");
	Console::write("init failed to load, code ");
	Console::writeLine(rc);
}
static void clockTaskBody() {
	for (;;) {
		g_bgwork++;
		arch::halt_or_hlt();   // sleep until the next interrupt
	}
}

// Register a scheduler kernel thread (idle/clock) as a process so it shows up in
// /proc, exactly as Linux lists its kthreads (e.g. [kworker], swapper).
static void registerKthread(Task* t, const char* name) {
	Process* p = ProcTable::alloc(0);
	if (!p)
		return;
	p->task = t;
	p->kthread = true;
	const char* a[] = { name, 0 };
	ProcTable::setCommand(p, a, 1);
}

// Build the physical frame allocator from the arch memory map, then hand it to
// the arch MMU to bring up kernel paging. Machine-independent: the page-table
// format and CR registers live behind <arch/mmu.h>.
void Kernel::initPaging() {
	unsigned top = arch::bootMemTop();
	g_frames.init(top);
	arch::bootMemForEachUsable(&g_frames, markFree);
	arch::mmuInitKernel(g_frames, top);
	Console::writeLine("paging enabled");

	// If the bootloader gave us a graphics framebuffer (vesafb model), map its MMIO
	// into the kernel now — before any per-process space is created — so we (and every
	// process) can draw into it. The framebuffer console takes over in a later stage.
	const arch::BootFramebuffer* fb = arch::bootFramebuffer();
	if (fb) {
		arch::mmuMapKernelMmio((uint32_t) fb->addr, fb->pitch * fb->height);
		// Stage B smoke: exercise the MI renderer + 8x16 font on the real framebuffer —
		// background, R/G/B bars, and a text line. (Stage C turns this into the console.)
		FbSurface surf = { (uint8_t*) (uint32_t) fb->addr, fb->pitch, fb->width, fb->height,
				fb->bpp };
		fbFillRect(surf, 0, 0, surf.width, surf.height, 0x00203A66);
		fbFillRect(surf, 0, 0,  surf.width, 16, 0x00CC3333);
		fbFillRect(surf, 0, 16, surf.width, 16, 0x0033CC33);
		fbFillRect(surf, 0, 32, surf.width, 16, 0x003333CC);
		const char* msg = "NanOS framebuffer + 8x16 font OK";
		uint32_t gx = 8;
		for (const char* p = msg; *p; p++, gx += FONT_W)
			fbBlitGlyph(surf, fontGlyph((unsigned char) *p), gx, 80, 0x00FFFFFF, 0x00203A66);
		Console::write("framebuffer: ");
		Console::write((int) fb->width);
		Console::write("x");
		Console::write((int) fb->height);
		Console::write(" bpp");
		Console::writeLine((int) fb->bpp);
	} else {
		Console::writeLine("framebuffer: none (VGA text)");
	}
}

void Kernel::start() {
	Console::clearScreen();
	Console::writeLine("NanoOS initialize...");

	// Bring up the CPU descriptor tables, interrupt vectors and legacy input (arch).
	arch::cpuInit();
	Console::writeLine("");

	// Enable paging (identity-mapped) before the storage stack / userspace.
	initPaging();
//	char* bb = buf;
	//kernel::Interrupt::registerInterruptHandler(, &callback3);

	// Storage stack: register the arch boot disk as a block device, register the
	// filesystem types, and mount at "/". All access goes through the VFS.
	BlockDevice* hd0 = arch::bootDisk();
	DeviceManager::registerDevice(hd0);
	Vfs* vfs = new Vfs();
	vfs->registerType(new Ext4FileSystemType());
	vfs->registerType(new Ext2FileSystemType());

	// The root "/" is a synthetic in-memory namespace (/disks, /dev, /proc); the
	// physical disk is NOT mounted at "/" but under /disks/main.
	SynthFs* root = new SynthFs();
	vfs->mount("/", root);
	mountVolume(vfs, root, "main", hd0, 2048);

	// Install the syscall interface over the VFS, then a boot sanity syscall.
	installSyscalls(vfs);
	arch::syscallSelfTest();

	// Start the scheduler: idle (task 0), init/nsh (task 1, enters ring 3), and a
	// background clock thread (task 2). The 1000 Hz timer preempts; init launches the
	// shell. Control never returns from start().
	g_vfs = vfs;
	Scheduler::init();
	Task* initTask = Scheduler::create(initTaskBody, 1);
	ProcTable::byPid(1)->task = initTask;   // the boot process (pid 1) runs the init task
	const char* initArgv[] = { "init", 0 };
	ProcTable::setCommand(ProcTable::byPid(1), initArgv, 1);   // until it execve's nsh
	Task* clockTask = Scheduler::create(clockTaskBody, 2);
	registerKthread(Scheduler::idle(), "idle");   // kernel threads visible in /proc
	registerKthread(clockTask, "clock");
	arch::archTimerInit(1000);
	Scheduler::start();

	for (;;) arch::halt_or_hlt();   // unreachable
}

void Kernel::loop() {}

}
;
