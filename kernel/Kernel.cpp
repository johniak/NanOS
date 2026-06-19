#include <stdint.h>
#include "Kernel.h"
#include "Console.h"
#include "Clock.h"
#include "BlockDevice.h"
#include "DeviceManager.h"
#include "UsbCore.h"            // USB enumeration (in-kernel storage path for live-USB root)
#include "UsbMsc.h"
#include "UsbMscBlockDevice.h"
#include "PartitionTable.h"     // MBR + GPT root-partition discovery
#include "UsbHidInput.h"        // in-kernel USB-HID keyboard/mouse -> existing evdev
#include "Vfs.h"
#include "Ext2Filesystem.h"
#include "Ext4Filesystem.h"
#include "SynthFs.h"
#include "RamFs.h"
#include "Fbdev.h"
#include "Fb0Device.h"
#include "KeyboardDevice.h"
#include "Pty.h"
#include "KernelExports.h"   // kernel symbols exported to loadable modules (nkext)
#include "KextLoader.h"      // load /nanos/kext/*.nkext at boot
#include "Pci.h"             // PCI bus enumeration (finds the NIC for the e1000 kext)
#include "NetCore.h"         // net stack bring-up: lo + RX softirq thread + driver exports
#include <arch/pci.h>
#include "SignalDispatch.h"   // consoleSignal (tty control keys -> foreground process)
#include "Csprng.h"           // csprngKernelSeed: seed the kernel CSPRNG at boot
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
#include <arch/usbhc.h>       // in-kernel USB host controller (root-storage path for live-USB)
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

// PTY terminal-generated signal (Ctrl+C/\/Z on the master) -> the tty's foreground process
// group (TIOCSPGRP), falling back to the single foreground pid when no group claimed it.
static void ptySignal(void*, int sig, int pgrp) {
	consoleSignalGroup(sig, pgrp);
}

// Mount a physical volume at /disks/<name> and register a marker under the synthetic
// /disks so it shows up in readdir. Single helper = one source of truth.
// Discover the first partition's start LBA from the MBR partition table, instead of
// hardcoding it. Reads sector 0; if it has the 0x55AA signature, returns the start LBA of
// the first non-empty partition entry. Falls back to 2048 (the image's GRUB layout) when
// there is no valid MBR — so a bare/unpartitioned image still mounts.
static unsigned firstPartitionLba(BlockDevice* dev) {
	return firstFsPartitionLba(dev);   // MBR + GPT (the image is GPT after the Limine switch)
}

// Discover USB mass-storage devices on the in-kernel USB host controller: enumerate each port,
// bind any Mass-Storage interface to a UsbMsc + UsbMscBlockDevice, register it, and return the
// first one found (the live-USB root candidate). No-op (returns null) if no controller / no device.
static BlockDevice* usbStorageDiscover() {
	BlockDevice* first = 0;
	for (int i = 0; i < usbDeviceCount(); i++) {
		const UsbDevice* dev = usbDeviceAt(i);
		bool isMsc = false;
		for (int j = 0; j < dev->numInterfaces; j++)
			if (dev->iface[j].bInterfaceClass == USB_CLASS_MASS_STORAGE)
				isMsc = true;
		if (!isMsc)
			continue;
		int epIn = 0, epOut = 0;
		for (int j = 0; j < dev->numEndpoints; j++) {
			unsigned char a = dev->endpoint[j].bEndpointAddress;
			if ((dev->endpoint[j].bmAttributes & 0x3) == 2) {   // bulk endpoint
				if (a & 0x80) epIn = a; else epOut = a;
			}
		}
		if (!epIn || !epOut)
			continue;
		UsbMsc* msc = new UsbMsc();
		if (usbMscInit(msc, dev->slot, epIn, epOut) != 0)
			continue;
		uint32_t blocks = 0, bsize = 0;
		if (usbMscReadCapacity(msc, &blocks, &bsize) != 0)
			continue;
		BlockDevice* bd = new UsbMscBlockDevice(msc, "usb0");
		DeviceManager::registerDevice(bd);
		if (!first)
			first = bd;
	}
	return first;
}

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

// One-shot read-write self-test of the persistent disk: prove the full stack — VFS -> ext
// write + JBD2 transaction -> AtaBlockDevice -> ATA PIO write -> the physical disk — by
// (re)writing a marker file and reading it back, and report whether last boot's marker survived
// (persistence across reboot). Mutates only /disks/main/nanos/rwtest; e2fsck-clean afterwards.
static void extRwSelftest(Vfs* vfs) {
	const char* path = "/disks/main/nanos/rwtest";
	const char* marker = "NANOS-RW-OK";
	unsigned mlen = (unsigned) strlen(marker);
	char prev[32];
	int pn = vfs->read(String(path), 31, 0, prev);
	bool persisted = pn == (int) mlen;
	for (unsigned i = 0; persisted && i < mlen; i++)
		if (prev[i] != marker[i]) persisted = false;

	int cr = vfs->create(String(path), 0644);
	int wr = (cr == 0) ? vfs->write(String(path), mlen, 0, marker) : cr;
	char back[32];
	int rn = vfs->read(String(path), 31, 0, back);
	bool ok = wr == (int) mlen && rn == (int) mlen;
	for (unsigned i = 0; ok && i < mlen; i++)
		if (back[i] != marker[i]) ok = false;

	Console::write("EXT-RW selftest: ");
	Console::write(ok ? "write+read OK" : "FAIL");
	Console::writeLine(persisted ? " (marker persisted from last boot)" : " (first write to this image)");
}

// Populate the writable /etc (RamFs) from the on-disk template /disks/main/nanos/config/etc/.
// Linux apps expect /etc/{resolv.conf,hosts,nsswitch.conf,protocols,services}; the disk is
// read-only, so we copy the templates into the tmpfs at boot (DHCP later rewrites resolv.conf).
static void populateEtc(Vfs* vfs) {
	static const char* files[] = { "resolv.conf", "hosts", "nsswitch.conf", "protocols", "services", "shells", 0 };
	char buf[512];
	for (int i = 0; files[i]; i++) {
		String src = String("/disks/main/nanos/config/etc/") + String(files[i]);
		String dst = String("/etc/") + String(files[i]);
		int n = vfs->read(src, sizeof(buf), 0, buf);
		if (n <= 0)
			continue;
		if (vfs->create(dst, 0644) == 0)
			vfs->write(dst, (unsigned) n, 0, buf);
	}
}

// Scheduler task bodies. Task 1 (init/nsh) enters ring 3 via execProgram; task 2 is
// a background kernel thread (demonstrates that several tasks coexist) — it sleeps and
// counts, so cat /proc/uptime advancing while the shell is idle proves the scheduler
// keeps running.
static Vfs* g_vfs = 0;

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

// Register a scheduler kernel thread (idle/clock) as a process so it shows up in
// /proc, exactly as Linux lists its kthreads (e.g. [kworker], swapper).
static void registerKthread(Task* t, const char* name) {
	Process* p = ProcTable::alloc(0);
	if (!p)
		return;
	ProcTable::bindTask(p, t, p->leaderThread());   // wire task<->process<->leader thread in one place
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

// Enumerate the PCI bus and log a one-line summary + the e1000 NIC if present. Purely
// informational at boot; the e1000 kext does its own Pci::find. Keeps the wire-up honest:
// if the NIC isn't found here, the driver phase has nothing to bind to.
static void pciScanReport() {
	PciDevice devs[32];
	int n = Pci::enumerate(devs, 32);
	Console::write("  PCI: ");
	Console::write(n);
	Console::write(" device(s); ");
	PciDevice nic;
	if (Pci::find(0x8086, 0x100E, nic)) {
		Console::write("e1000 8086:100E @ ");
		Console::write((int) nic.bus); Console::write(":");
		Console::write((int) nic.dev); Console::write(".");
		Console::write((int) nic.func);
		Console::write(" BAR0=");
		Console::writeHex((int) nic.bar[0].addr);
		Console::write(" irq=");
		Console::write((int) nic.irqLine);
		Console::writeLine("");
	} else {
		Console::writeLine("no e1000 (run with: make run-net)");
	}
}

void Kernel::start() {
	Console::clearScreen();
	Console::writeLine("NanoOS initialize...");

	// Bring up the CPU descriptor tables, interrupt vectors and legacy input (arch).
	arch::cpuInit();
	setBootEpoch(arch::rtcEpoch());   // seed the wall clock from the RTC (file timestamps)
	csprngKernelSeed();               // seed the kernel CSPRNG (RDRAND+jitter+RTC) before any RNG use
	Console::writeLine("");

	// Enable paging (identity-mapped) before the storage stack / userspace.
	initPaging();
//	char* bb = buf;
	//kernel::Interrupt::registerInterruptHandler(, &callback3);

	// PCI config-space backend must be live before BOTH the USB host controller (below) and the
	// e1000 kext (later). Installed here, before the storage stack, so the in-kernel USB path can
	// discover its controller. (Idempotent: the later kexts rely on this same backend.)
	Pci::setBackend(arch::pciConfigRead32, arch::pciConfigWrite32);

	// Bring up the in-kernel USB host controller (xHCI) BEFORE the storage stack. On a live-USB
	// system the root filesystem lives on a USB mass-storage device, so the xHCI+USB-core+MSC path
	// is compiled into the kernel (same root-driver-in-kernel rule as ATA). No-op if absent.
	okBegin("USB host controller (xHCI)");
	arch::usbHostInit();
	usbEnumerateAll();   // enumerate every USB port once into the registry (storage + HID read it)
	okEnd();

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
	kernelExportsInit(root);   // loadable modules add /dev/input<N> through this root
	// Root discovery: a live-USB system keeps the whole FS on a USB stick, so prefer a USB
	// mass-storage volume with a valid MBR; otherwise fall back to the ATA disk (the QEMU
	// -drive image path). The USB storage path (xHCI+core+MSC) was brought up before paging.
	BlockDevice* usb0 = usbStorageDiscover();
	BlockDevice* rootDev = hd0;
	if (usb0) {
		unsigned char mbr[512];
		if (usb0->readSectors(0, 1, mbr) == 0 && mbr[510] == 0x55 && mbr[511] == 0xAA) {
			rootDev = usb0;
			Console::writeLine("Root: USB mass-storage device (usb0)");
		}
	}
	okBegin("Mounting ext filesystem at /disks/main");
	mountVolume(vfs, root, "main", rootDev, firstPartitionLba(rootDev));   // USB-or-ATA, MBR-discovered
	okEnd();

	// Phase 6: exercise the read-write path on the real disk and report persistence.
	extRwSelftest(vfs);

	// Writable in-memory filesystem (tmpfs) at /tmp, the Unix way to give programs a
	// place to write transient files (e.g. Doom's config + savegames). Cleared on reboot.
	okBegin("Mounting tmpfs at /tmp");
	vfs->mount("/tmp", new RamFs());
	okEnd();

	// Writable /etc (RamFs), populated from the read-only on-disk template, so Linux network
	// apps find /etc/{resolv.conf,hosts,nsswitch.conf,...} at the canonical path (DHCP rewrites
	// resolv.conf in FAZA 10). The disk driver is read-only, so /etc lives in a tmpfs like /tmp.
	okBegin("Mounting /etc (tmpfs) + config");
	vfs->mount("/etc", new RamFs());
	populateEtc(vfs);
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
	// /dev/tty = the controlling terminal. With one pty it is the same slave as pts0, so a
	// program (bash) can open("/dev/tty") to reach its terminal without knowing the pts name.
	root->addChar(root->dev(), "tty", new PtySlave(pty), 0666);
	okEnd();

	// PCI bus: install the arch config-space backend (0xCF8/0xCFC) and scan. Must run BEFORE
	// the kexts load, since the e1000 NIC driver finds its device through kernel::Pci / the
	// knx_pci_* exports.
	okBegin("PCI bus enumeration");
	okEnd();
	pciScanReport();

	// Load kernel modules (nkext) from /nanos/kext — the PS/2 keyboard + mouse drivers live
	// here, NOT in the kernel image. Each registers its IRQ + /dev node from its nkext_init().
	okBegin("Loading kernel modules /nanos/kext");
	loadAllKexts(vfs, "/disks/main/nanos/kext");
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
	okBegin("Scheduler + init task + idle thread");
	Scheduler::init();
	Task* initTask = Scheduler::create(initTaskBody, 1);
	Process* p1 = ProcTable::byPid(1);
	ProcTable::bindTask(p1, initTask, p1->leaderThread());   // pid 1 runs the init task (leader thread)
	const char* initArgv[] = { "init", 0 };
	ProcTable::setCommand(p1, initArgv, 1);   // until it execve's nsh
	registerKthread(Scheduler::idle(), "idle");   // the idle kernel thread, visible in /proc
	okEnd();

	// Net stack: lo + the RX softirq thread (drains the backlog outside IRQ) + the periodic timer
	// thread + the driver hooks; then configure eth0 + the default route. Any frames the e1000
	// queued during kext load drain on the softirq's first run. udhcpc (run by init) refines this.
	okBegin("Networking: lo + eth0 + RX softirq + net-timer");
	registerKthread(netCoreInit(), "ksoftirqd-net");
	registerKthread(netTimerThread(), "net-timer");
	netBringUp();
	okEnd();
	Console::writeLine("       eth0 10.0.2.15/24 gw 10.0.2.2 (static; udhcpc refines it at init)");

	// USB-HID input: if a USB keyboard/mouse enumerated, start the poll thread feeding the
	// existing evdev devices (keyboard -> /dev/input0, mouse -> /dev/input<N>). NanWM unchanged.
	okBegin("USB-HID input (keyboard/mouse)");
	usbHidInit();
	okEnd();

	okBegin("Timer 1000 Hz + starting shell/services");
	arch::archTimerInit(1000);
	okEnd();
	Scheduler::start();

	for (;;) arch::halt_or_hlt();   // unreachable
}

void Kernel::loop() {}

}
;
