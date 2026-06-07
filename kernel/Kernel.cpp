#include <stdint.h>
#include "Kernel.h"
#include "Console.h"
#include "BlockDevice.h"
#include "DeviceManager.h"
#include "Vfs.h"
#include "Ext2Filesystem.h"
#include "Ext4Filesystem.h"
#include "SynthFs.h"
#include "Scheduler.h"
#include <arch/sched.h>
#include "Syscall.h"
#include "SyscallDispatch.h"
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
	int rc = execProgram(g_vfs, "/disks/main/nanos/core/init.nxe");
	Console::write("init exited with code ");
	Console::writeLine(rc);
}
static void clockTaskBody() {
	for (;;) {
		g_bgwork++;
		arch::halt_or_hlt();   // sleep until the next interrupt
	}
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
	Scheduler::create(initTaskBody, 1);
	Scheduler::create(clockTaskBody, 2);
	arch::archTimerInit(1000);
	Scheduler::start();

	for (;;) arch::halt_or_hlt();   // unreachable
}

void Kernel::loop() {}

}
;
