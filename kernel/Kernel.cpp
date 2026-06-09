#include <stdint.h>
#include "Kernel.h"
#include "Console.h"
#include "BlockDevice.h"
#include "DeviceManager.h"
#include "Vfs.h"
#include "Ext2Filesystem.h"
#include "Ext4Filesystem.h"
#include "SynthFs.h"
#include "RamFs.h"
#include "Fbdev.h"
#include "Fb0Device.h"
#include "KeyboardDevice.h"
#include "Pty.h"
#include "SignalDispatch.h"   // consoleSignal (tty control keys -> foreground process)
#include "Scheduler.h"
#include <arch/sched.h>
#include "Syscall.h"
#include "SyscallDispatch.h"
#include "Process.h"
#include "Exec.h"
#include "List.h"
#include "String.h"
#include <arch/bootinfo.h>
#include <arch/console.h>
#include <arch/mmu.h>
#include <arch/cpu.h>
#include <arch/syscall.h>
#include <arch/block.h>
#include "FrameAllocator.h"
#include "memory_manager.h"   // heapTotalBytes/heapFreeBytes for /proc/meminfo
char buf[1024];

namespace kernel {

// Live system memory figures (kB) for /proc/meminfo. MemTotal is the whole RAM from the
// boot map; MemFree is the free physical page frames; KHeap* is the kernel byte heap.
unsigned sysMemTotalKb() { return (unsigned) (arch::bootMemTop() / 1024u); }
unsigned sysMemFreeKb()  { return (unsigned) (g_frames.freeCount() * (FRAME_SIZE / 1024u)); }
unsigned sysHeapTotalKb() { return heapTotalBytes() / 1024u; }
unsigned sysHeapFreeKb()  { return heapFreeBytes() / 1024u; }

// Mark a usable physical range free in the frame allocator (arch reports only
// usable ranges via <arch/bootinfo.h>).
static void markFree(void* fa, uint64_t base, uint64_t len) {
	((FrameAllocator*) fa)->markRangeFree((uint32_t) base, (uint32_t) len);
}

// PTY terminal-generated signal (Ctrl+C/\/Z on the master) -> the foreground process.
// (Stage 4 will route by process group; for now consoleSignal targets the foreground pid.)
static void ptySignal(void*, int sig, int /*pgrp*/) {
	consoleSignal(sig);
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
	// Let the "[ OK ]" boot splash sit for ~2s (the timer is running now), then clear to a
	// fresh terminal — the Linux feel of a boot screen handing off to a login/shell.
	unsigned t0 = Scheduler::ticks();
	while (Scheduler::ticks() - t0 < 2000)
		Scheduler::ioWait();   // yield + re-wake each tick (deferred model: kthreads must yield)
	Console::clearScreen();

	// Enters ring 3 and does not return on success; only reached if the load fails.
	int rc = execProgram(g_vfs, "/disks/main/nanos/core/init.nxe");
	Console::write("init failed to load, code ");
	Console::writeLine(rc);
}
static void clockTaskBody() {
	for (;;) {
		g_bgwork++;
		Scheduler::ioWait();   // yield + re-wake each tick (kthreads yield voluntarily now)
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

// Linux-style boot status: print the line with a blank marker first, run the step, then
// overwrite the marker with a green OK in place (carriage-return back to column 0). The
// step between okBegin and okEnd must NOT print a newline.
static void okBegin(const char* msg) {
	Console::write("[    ] ");
	Console::write(msg);
}
static void okEnd() {
	Console::write("\r[ \033[1;32mOK\033[0m ]\n");
}

// Build the physical frame allocator from the arch memory map, then hand it to
// the arch MMU to bring up kernel paging. Machine-independent: the page-table
// format and CR registers live behind <arch/mmu.h>.
void Kernel::initPaging() {
	unsigned top = arch::bootMemTop();
	g_frames.init(top);
	arch::bootMemForEachUsable(&g_frames, markFree);
	arch::mmuInitKernel(g_frames, top);

	// If the bootloader gave us a graphics framebuffer (vesafb model), map its MMIO into
	// the kernel now — before any per-process space is created, so the mapping is shared —
	// and hand the console over to it (Linux fbcon style). Boot text from here on renders
	// as pixel glyphs on the framebuffer.
	const arch::BootFramebuffer* fb = arch::bootFramebuffer();
	if (fb) {
		arch::mmuMapKernelMmio((uint32_t) fb->addr, fb->pitch * fb->height);
		arch::consoleActivateFramebuffer();
	}
	// Boot splash (now that the framebuffer console is up). Subsystems that came up
	// before the framebuffer (CPU/interrupts) are acknowledged here in order.
	Console::writeLine("");
	Console::writeLine("    NanOS  --  booting");
	Console::writeLine("");
	okBegin("CPU, GDT/IDT, interrupts, keyboard"); okEnd();
	okBegin("Paging enabled"); okEnd();
	if (fb) {
		okBegin("Framebuffer ");
		Console::write((int) fb->width);
		Console::write("x");
		Console::write((int) fb->height);
		Console::write("x");
		Console::write((int) fb->bpp);
		okEnd();
	} else {
		okBegin("Framebuffer: none (VGA text)"); okEnd();
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
	okBegin("Mounting ext filesystem at /disks/main");
	mountVolume(vfs, root, "main", hd0, 2048);
	okEnd();

	// Writable in-memory filesystem (tmpfs) at /tmp, the Unix way to give programs a
	// place to write transient files (e.g. Doom's config + savegames). Cleared on reboot.
	okBegin("Mounting tmpfs at /tmp");
	vfs->mount("/tmp", new RamFs());
	okEnd();

	// Expose the framebuffer as Linux /dev/fb0 (fbdev ioctls + mmap + read/write) so
	// framebuffer software can drive it. Only when the bootloader gave us a framebuffer.
	const arch::BootFramebuffer* fbdev = arch::bootFramebuffer();
	if (fbdev) {
		okBegin("Graphics device /dev/fb0");
		FbInfo info = { (uint32_t) fbdev->addr, fbdev->pitch, fbdev->width,
				fbdev->height, fbdev->bpp };
		root->addChar(root->dev(), "fb0", new Fb0Device(info), 0666);
		okEnd();
	}

	// Expose the keyboard as Linux-style /dev/input0 (evdev): the PS/2 IRQ feeds it
	// scancodes, it queues key down/up events, programs read() them. The arch input path
	// feeds it once kbdRegister() points at it.
	okBegin("Input device /dev/input0");
	KeyboardDevice* kbd = new KeyboardDevice();
	kbdRegister(kbd);
	root->addChar(root->dev(), "input0", kbd, 0444);
	okEnd();

	// Pseudo-terminal: /dev/ptmx (master, held by the userspace terminal emulator) +
	// /dev/pts0 (slave, the shell's controlling tty). One pair for the single console for
	// now. Ctrl+C etc. on the master route to the foreground process via consoleSignal.
	okBegin("PTY /dev/ptmx + /dev/pts0");
	Pty* pty = new Pty();
	pty->setSignalFn(ptySignal, 0);
	root->addChar(root->dev(), "ptmx", new PtyMaster(pty), 0666);
	root->addChar(root->dev(), "pts0", new PtySlave(pty), 0666);
	okEnd();

	// Install the syscall interface over the VFS, then a (silent) boot sanity syscall.
	okBegin("Syscall interface (int 0x80)");
	installSyscalls(vfs);
	arch::syscallSelfTest();
	okEnd();

	// Start the scheduler: idle (task 0), init/nsh (task 1, enters ring 3), and a
	// background clock thread (task 2). The 1000 Hz timer preempts; init launches the
	// shell. Control never returns from start().
	g_vfs = vfs;
	okBegin("Scheduler + tasks; starting shell");
	Scheduler::init();
	Task* initTask = Scheduler::create(initTaskBody, 1);
	ProcTable::byPid(1)->task = initTask;   // the boot process (pid 1) runs the init task
	const char* initArgv[] = { "init", 0 };
	ProcTable::setCommand(ProcTable::byPid(1), initArgv, 1);   // until it execve's nsh
	Task* clockTask = Scheduler::create(clockTaskBody, 2);
	registerKthread(Scheduler::idle(), "idle");   // kernel threads visible in /proc
	registerKthread(clockTask, "clock");
	arch::archTimerInit(1000);
	okEnd();
	Scheduler::start();

	for (;;) arch::halt_or_hlt();   // unreachable
}

void Kernel::loop() {}

}
;
