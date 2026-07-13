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
#include "NwShmDevice.h"
#include "KeyboardDevice.h"
#include "Pty.h"
#include "KernelExports.h"   // kernel symbols exported to loadable modules (nkext)
#include "KextLoader.h"      // load /nanos/kext/*.nkext at boot
#include "Pci.h"             // PCI bus enumeration (finds the NIC for the e1000 kext)
#include "NetCore.h"         // net stack bring-up: lo + RX softirq thread + driver exports
#include <arch/pci.h>
#include "SignalDispatch.h"   // consoleSignal (tty control keys -> foreground process)
#include "vt/VtManager.h"     // virtual terminals: build the manager + register /dev/ttyN
#include "VtTty.h"            // /dev/tty1..7 + /dev/tty0 + /dev/tty + /dev/console
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
#include <arch/smp.h>         // SMP: ACPI CPU enumeration + application-processor bring-up
#include "FrameAllocator.h"
#include "memory_manager.h"   // heapTotalBytes/heapFreeBytes for /proc/meminfo
char buf[1024];

namespace kernel {

// Total usable RAM in bytes — the SUM of the boot map's usable regions, NOT bootMemTop() (the
// highest usable *address*). On real hardware the span from low RAM up to the top is riddled with
// PCI MMIO, ACPI/EFI-reserved and GPU-stolen holes; using the top address made MemTotal-MemFree
// count those gigabytes of holes as "used" (~2.5 GiB phantom on a real laptop, near-0 in QEMU).
// Computed once at boot in the free-marking pass (markFreeAndCount). bootMemTop() stays the top
// address — correct for sizing the frame bitmap and the identity map.
uint64_t g_usableRamBytes = 0;

// Spinlock wedge tripwire (Spinlock.h). Null until the VT manager is up (armed below in
// kernelMain); host unit tests never arm it, so the contended-spin check stays a no-op there.
void (*g_spinStallSink)(const void* ra) = nullptr;

// Live system memory figures (kB) for /proc/meminfo. MemTotal is total usable RAM; MemFree is the
// free physical page frames; KHeap* is the kernel byte heap.
unsigned sysMemTotalKb() {
	uint64_t b = g_usableRamBytes ? g_usableRamBytes : arch::bootMemTop();   // fallback before boot pass
	return (unsigned) (b / 1024ull);
}
unsigned sysMemFreeKb()  { return (unsigned) (g_frames.freeCount() * (FRAME_SIZE / 1024u)); }
unsigned sysHeapTotalKb() { return heapTotalBytes() / 1024u; }
unsigned sysHeapFreeKb()  { return heapFreeBytes() / 1024u; }

// Mark a usable physical range free in the frame allocator AND accumulate total usable RAM
// (arch reports only usable ranges via <arch/bootinfo.h>). The length is clamped to the
// frame-pool span so MemTotal stays consistent with MemFree (both bounded by the bitmap).
struct UsableScan { FrameAllocator* fa; uint64_t top; };
static void markFreeAndCount(void* c, uint64_t base, uint64_t len) {
	UsableScan* s = (UsableScan*) c;
	s->fa->markRangeFree(base, len);   // 64-bit: a range based >4 GiB must not wrap
	uint64_t end = base + len;
	if (end > s->top) end = s->top;    // drop anything beyond the bitmap-tracked span
	if (end > base)   g_usableRamBytes += end - base;
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

// ---- Boot-time USB/root diagnostics ----
// Tee the storage-discovery + root-mount decisions to the screen AND accumulate them into a buffer
// flushed to /disks/main/nanos/logs/boot-usb.txt once the root mounts, so a Dell USB boot leaves a
// log we can pull over pendrak instead of photographing the panel. If the root never mounts (the
// failure we are chasing) the screen copy is the fallback — see how the i915 harness logs the same
// way via knx_file_append.
static char g_bootDiag[4096];
static unsigned g_bootDiagLen = 0;
static void bdPut(const char* s) {
	for (const char* p = s; *p && g_bootDiagLen < sizeof(g_bootDiag) - 1; p++)
		g_bootDiag[g_bootDiagLen++] = *p;
	Console::write(s);
}
static void bdNum(long v) {
	if (v < 0) { bdPut("-"); v = -v; }
	if (!v) { bdPut("0"); return; }
	char t[24]; int i = 0; unsigned long u = (unsigned long) v;
	while (u) { t[i++] = (char) ('0' + u % 10); u /= 10; }
	char r[24]; int j = 0; while (i) r[j++] = t[--i]; r[j] = 0;
	bdPut(r);
}
// Best-effort persist: only lands if /disks/main mounted writable. Rewrites from offset 0 with the
// whole (monotonically growing) buffer, so later calls supersede earlier ones.
static void bdFlushToDisk(Vfs* vfs) {
	if (!vfs) return;
	String path("/disks/main/nanos/logs/boot-usb.txt");
	vfs->create(path, 0644);   // create if absent; harmless if it already exists
	vfs->write(path, g_bootDiagLen, 0, g_bootDiag);
}

// Discover USB mass-storage devices on the in-kernel USB host controller: enumerate each port,
// bind any Mass-Storage interface to a UsbMsc + UsbMscBlockDevice, register it, and return the
// first one found (the live-USB root candidate). No-op (returns null) if no controller / no device.
static BlockDevice* usbStorageDiscover() {
	BlockDevice* first = 0;
	bdPut("== boot-usb == usbDeviceCount="); bdNum(usbDeviceCount()); bdPut("\n");
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
		// A removable gadget/stick asserts UNIT ATTENTION on first access and answers CHECK CONDITION
		// until a REQUEST SENSE clears it. Clear it (TEST UNIT READY + REQUEST SENSE) and retry READ
		// CAPACITY, so the device isn't wrongly skipped here (which falls back to a nonexistent ATA
		// disk on a USB-boot Dell → "init failed to load"). A fast flash stick is ready immediately,
		// so this is a no-op for it.
		uint8_t sk = 0, asc = 0;
		usbMscRequestSense(msc, &sk, &asc);        // drain any pending sense first
		int ready = usbMscWaitReady(msc, 16);
		bdPut("  MSC slot="); bdNum(dev->slot);
		bdPut(" epIn="); bdNum(epIn); bdPut(" epOut="); bdNum(epOut);
		bdPut(" ready="); bdNum(ready); bdPut(" sense="); bdNum(sk); bdPut("/"); bdNum(asc);
		uint32_t blocks = 0, bsize = 0;
		int rcCap = -1;
		for (int rtry = 0; rtry < 4 && rcCap != 0; rtry++) {
			rcCap = usbMscReadCapacity(msc, &blocks, &bsize);
			if (rcCap != 0) usbMscRequestSense(msc, 0, 0);   // clear CHECK CONDITION between tries
		}
		bdPut(" readCap="); bdNum(rcCap);
		if (rcCap != 0) {
			bdPut(" FAIL phase="); bdNum(g_usbMscFailPhase);
			bdPut(" csw="); bdNum(g_usbMscCswStatus);
			bdPut(" moved="); bdNum(g_usbMscDataMoved); bdPut("\n");
			continue;
		}
		bdPut(" blocks="); bdNum((long) blocks); bdPut(" bsize="); bdNum((long) bsize); bdPut("\n");
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
	static const char* files[] = { "resolv.conf", "hosts", "nsswitch.conf", "protocols", "services", "shells", "profile", 0 };
	char buf[2048];
	for (int i = 0; files[i]; i++) {
		String src = String("/disks/main/nanos/config/etc/") + String(files[i]);
		String dst = String("/etc/") + String(files[i]);
		int n = vfs->read(src, sizeof(buf), 0, buf);
		if (n <= 0)
			continue;
		if (vfs->create(dst, 0644) == 0)
			vfs->write(dst, (unsigned) n, 0, buf);
	}
	// The account database: /etc/{passwd,group,shadow,sudoers} are loaded into the writable /etc
	// tmpfs from the persistent on-disk copies under /nanos/config. shadow is root-only (0600) and
	// sudoers 0440 so an unprivileged user cannot read hashes or the sudo policy. (Runtime edits
	// live in tmpfs; the persistent source is /disks/main/nanos/config, regenerated each boot.)
	static const char* acct[] = { "passwd", "group", "shadow", "sudoers", 0 };
	static const unsigned acctMode[] = { 0644u, 0644u, 0600u, 0440u };
	for (int i = 0; acct[i]; i++) {
		String src = String("/disks/main/nanos/config/") + String(acct[i]);
		String dst = String("/etc/") + String(acct[i]);
		int n = vfs->read(src, sizeof(buf), 0, buf);
		if (n <= 0)
			continue;
		if (vfs->create(dst, acctMode[i]) == 0) {
			vfs->write(dst, (unsigned) n, 0, buf);
			vfs->chmod(dst, acctMode[i]);   // create masks via umask; force the intended mode
			vfs->chown(dst, 0, 0);          // owned by root
		}
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
	// Persist the failure to the pullable boot log too (best-effort; supersedes the mount-time flush).
	bdPut("init exec /disks/main/nanos/core/init.nxe rc="); bdNum(rc); bdPut("\n");
	bdFlushToDisk(g_vfs);
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
	uint64_t top = arch::bootMemTop();   // 64-bit top-of-RAM (capped at the 16 GiB frame-pool capacity)
	g_frames.init(top);
	UsableScan scan = { &g_frames, top };
	arch::bootMemForEachUsable(&scan, markFreeAndCount);   // free each usable range + sum total usable RAM
	arch::mmuInitKernel(g_frames, top);  // builds the full huge-page map, swaps CR3, brings up LAPIC

	// If the bootloader gave us a graphics framebuffer (vesafb model), map its MMIO into
	// the kernel now — before any per-process space is created, so the mapping is shared —
	// and hand the console over to it (Linux fbcon style). Boot text from here on renders
	// as pixel glyphs on the framebuffer.
	const arch::BootFramebuffer* fb = arch::bootFramebuffer();
	if (fb) {
		arch::mmuMapKernelMmio(fb->addr, fb->pitch * fb->height);   // fb->addr is 64-bit: real HW puts the LFB >4 GiB
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

	// Display-adapter inventory (i915 plan Task 1): for every PCI display-class device (class 0x03)
	// print location, id, and BAR0/BAR2 base+size. For Intel IGPs (vendor 0x8086) also dump the
	// graphics-specific config registers the i915 kext needs to build its device: GGC (0x50, stolen
	// size/pre-alloc bits), BDSM (0x5C, data-stolen-memory base), and ASLS (0xFC, OpRegion pointer).
	// These are read straight from config space so the Dell values can be transcribed into the test
	// log before any driver code binds. Harmless on QEMU (virtio-gpu is display-class too).
	for (int i = 0; i < n; ++i) {
		const PciDevice& g = devs[i];
		if (g.classCode != 0x03) continue;   // 0x03 = display controller
		Console::write("  DISPLAY ");
		Console::writeHex((int) g.vendor); Console::write(":"); Console::writeHex((int) g.device);
		Console::write(" @ ");
		Console::write((int) g.bus); Console::write(":");
		Console::write((int) g.dev); Console::write(".");
		Console::write((int) g.func);
		Console::write(" class="); Console::writeHex((int) g.classCode);
		Console::write("/"); Console::writeHex((int) g.subclass);
		Console::write(" BAR0="); Console::writeHex((uint64_t) g.bar[0].addr);
		Console::write("(sz="); Console::writeHex((uint64_t) g.bar[0].size); Console::write(")");
		Console::write(" BAR2="); Console::writeHex((uint64_t) g.bar[2].addr);
		Console::write("(sz="); Console::writeHex((uint64_t) g.bar[2].size); Console::write(")");
		Console::writeLine("");
		if (g.vendor == 0x8086) {
			uint16_t ggc  = Pci::read16(g.bus, g.dev, g.func, 0x50);
			uint32_t bdsm = Pci::read32(g.bus, g.dev, g.func, 0x5C);
			uint32_t asls = Pci::read32(g.bus, g.dev, g.func, 0xFC);
			Console::write("    Intel IGP: GGC="); Console::writeHex((uint64_t) ggc);
			Console::write(" BDSM="); Console::writeHex((uint64_t) bdsm);
			Console::write(" ASLS="); Console::writeHex((uint64_t) asls);
			Console::writeLine("");
		}
	}
}

void Kernel::start() {
	Console::clearScreen();
	Console::writeLine("Nano OS initialize...");

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
		int rcMbr = usb0->readSectors(0, 1, mbr);
		bdPut("root: usb0 present, MBR read="); bdNum(rcMbr);
		if (rcMbr == 0) { bdPut(" sig="); bdNum(mbr[510]); bdPut(","); bdNum(mbr[511]); }
		bdPut("\n");
		if (rcMbr == 0 && mbr[510] == 0x55 && mbr[511] == 0xAA) {
			rootDev = usb0;
			Console::writeLine("Root: USB mass-storage device (usb0)");
			bdPut("root: -> usb0 SELECTED\n");
		} else {
			bdPut("root: -> usb0 REJECTED, fallback to ATA hd0\n");
		}
	} else {
		bdPut("root: usb0 == NULL (no MSC accepted) -> fallback to ATA hd0\n");
	}
	unsigned rootLba = firstPartitionLba(rootDev);
	bdPut("mount /disks/main lba="); bdNum((long) rootLba); bdPut("\n");
	okBegin("Mounting ext filesystem at /disks/main");
	mountVolume(vfs, root, "main", rootDev, rootLba);   // USB-or-ATA, MBR-discovered
	okEnd();
	// First point the root is (supposedly) writable: try to persist the diagnostics so far. If this
	// lands, pendrak `make pull-files-pi PATHS=/nanos/logs` retrieves it; if the mount failed, it
	// silently no-ops and the screen copy is all we get.
	bdFlushToDisk(vfs);

	// Phase 6: exercise the read-write path on the real disk and report persistence.
	extRwSelftest(vfs);

	// Writable in-memory filesystem (tmpfs) at /tmp, the Unix way to give programs a
	// place to write transient files (e.g. Doom's config + savegames). Cleared on reboot.
	okBegin("Mounting tmpfs at /tmp");
	vfs->mount("/tmp", new RamFs(01777));   // sticky + world-writable so non-root can create temp files
	okEnd();

	// Writable /run (RamFs), the runtime-state dir (sudo's auth-timestamp dir /run/sudo, pid files,
	// ...). Like /tmp it is a transient tmpfs cleared on reboot; the synthetic root is read-only so
	// programs that expect /run to be writable (sudo) need a real mount here.
	okBegin("Mounting tmpfs at /run");
	vfs->mount("/run", new RamFs());
	okEnd();

	// Writable /etc (RamFs), populated from the read-only on-disk template, so Linux network
	// apps find /etc/{resolv.conf,hosts,nsswitch.conf,...} at the canonical path (DHCP rewrites
	// resolv.conf in FAZA 10). The disk driver is read-only, so /etc lives in a tmpfs like /tmp.
	okBegin("Mounting /etc (tmpfs) + config");
	vfs->mount("/etc", new RamFs());
	populateEtc(vfs);
	okEnd();

	// Virtual terminals: build the VT manager over the bootloader framebuffer. From here on the
	// kernel console is VT1 (consolePutChar -> tty1), Ctrl+Alt+Fn switches consoles, and the
	// per-VT line discipline/job control replaces the old console singleton. The per-pid signal
	// sender drives the graphics VT's VT_SETMODE release/acquire handshake. No-op without a
	// framebuffer (VGA-text-only boot keeps the legacy single console path).
	{
		const arch::BootFramebuffer* fbv = arch::bootFramebuffer();
		if (fbv) {
			okBegin("Virtual terminals tty1..tty7");
			kernel::FbSurface s = { (uint8_t*) (uintptr_t) fbv->addr, fbv->pitch,
					fbv->width, fbv->height, fbv->bpp };
			// Heap-allocate so the constructor actually RUNS (NanOS runs no global ctors): this is
			// the Vfs/filesystem pattern. A file-scope global would leave member ctors unrun —
			// notably the RecursiveSpinlock's ownerCpu=-1 sentinel — and self-deadlock on first lock.
			kernel::VtManager* vtmgr = new kernel::VtManager();
			vtmgr->init(s, [](int pid, int sig) { kernel::signalSend(pid, sig); },
					arch::consoleSerialOut);     // mirror the visible VT + kernel console to the serial log
			kernel::g_vtmgr = vtmgr;
			okEnd();

			// Arm the spinlock wedge tripwire (Spinlock.h): a waiter starved for ~10^9 pause
			// iterations (several seconds — no honest critical section) forces the panel back to
			// a text VT and prints its site ON SCREEN. This is the evidence channel of last
			// resort for a wedged USB/FS lock: every file-backed log is itself behind that lock,
			// so a Dell freeze with silent logs was undiagnosable — now it photographs.
			kernel::g_spinStallSink = [](const void* ra) {
				static volatile int once = 0;
				if (__atomic_exchange_n(&once, 1, __ATOMIC_ACQ_REL))
					return;
				if (kernel::g_vtmgr)
					kernel::g_vtmgr->panicSwitchToText();
				kernel::Console::write("\n*** SPINLOCK STALL >~5s ra=");
				kernel::Console::writeHex((unsigned long) (uintptr_t) ra);
				kernel::Console::writeLine(" — a kernel lock is wedged (holder never released)."
						" Photograph this screen. ***");
			};

			// /dev nodes for the consoles: tty1..tty7 (per-VT), tty0 (the active VT), tty (the
			// caller's controlling VT), console (the kernel console = VT1). login/getty opens
			// /dev/ttyN, dups it to 0/1/2, and TIOCSCTTYs it (init, Phase 4).
			okBegin("Console devices /dev/tty0..7,tty,console");
			for (int i = 1; i <= kernel::kVtCount; i++) {
				char nm[6] = { 't', 't', 'y', (char) ('0' + i), 0, 0 };
				// 0666: any logged-in user can open their VT by name (Linux chowns ttyN to the user
				// at login; NanOS's /dev SynthFs has no per-node chown, so a permissive mode is the
				// equivalent — matches /dev/tty, /dev/ptmx, /dev/pts0). Without it a non-root login
				// shell (bash as jan) cannot reopen its tty (ttyname) for readline and exits on EOF.
				root->addChar(root->dev(), nm, new kernel::VtTty(i), 0666);
			}
			root->addChar(root->dev(), "tty0", new kernel::VtTty(0), 0666);
			root->addChar(root->dev(), "console", new kernel::VtTty(1), 0600);
			okEnd();
		}
	}

	// Expose the framebuffer as Linux /dev/fb0 (fbdev ioctls + mmap + read/write) so
	// framebuffer software can drive it. Only when the bootloader gave us a framebuffer.
	const arch::BootFramebuffer* fbdev = arch::bootFramebuffer();
	if (fbdev) {
		okBegin("Graphics device /dev/fb0");
		FbInfo info = { fbdev->addr, fbdev->pitch, fbdev->width,
				fbdev->height, fbdev->bpp };
		root->addChar(root->dev(), "fb0", new Fb0Device(info), 0666);
		okEnd();
	}

	// Shared pixel buffers for the nanowm window pipeline (/dev/nwshm): clients allocate
	// their window surfaces here and commits carry coordinates only, not pixels.
	okBegin("Window shm device /dev/nwshm");
	root->addChar(root->dev(), "nwshm", new NwShmDevice(), 0666);
	okEnd();

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
	// /dev/tty = the controlling terminal, resolved per-caller. ControllingTty forwards to the
	// process's Process::cttyDev — the VtTty a getty adopted (TIOCSCTTY on /dev/ttyN) OR the
	// PtySlave an nterm/ssh shell adopted (TIOCSCTTY on /dev/pts0) — so /dev/tty is unified across
	// VTs and the pty. Without a framebuffer (no VTs) the only ctty is the pty, but ControllingTty
	// still routes there once the shell TIOCSCTTYs it; keep the direct PtySlave as the legacy
	// fallback for that headless path.
	if (kernel::g_vtmgr)
		root->addChar(root->dev(), "tty", new kernel::ControllingTty(), 0666);
	else
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
	kernelExportsSetVfs(vfs);   // knx_file_read (request_firmware) reads through this VFS
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

	// If a display kext (virtio_gpu) registered a framebuffer present callback during
	// loadAllKexts, spawn its present thread now (the scheduler exists from here on).
	kernel::fbStartPresentThread();
	// Run any kext callbacks deferred to post-scheduler (LinuxKPI workqueue/timer workers).
	kernel::runAfterSchedulerHooks();
	Console::writeLine("       eth0 10.0.2.15/24 gw 10.0.2.2 (static; udhcpc refines it at init)");

	// USB-HID input: if a USB keyboard/mouse enumerated, start the poll thread feeding the
	// existing evdev devices (keyboard -> /dev/input0, mouse -> /dev/input<N>). nanowm unchanged.
	okBegin("USB-HID input (keyboard/mouse)");
	usbHidInit();
	okEnd();

	okBegin("Timer 1000 Hz + starting shell/services");
	arch::archTimerInit(1000);
	okEnd();

	// SMP: enumerate CPUs (ACPI MADT) and INIT-SIPI-SIPI the application processors. For now the
	// APs idle (no ApEntry registered); Phase 3 points them at the scheduler. Uniprocessor / no
	// ACPI returns 1. Done after the timer + LAPIC are up, before the scheduler starts.
	okBegin("SMP: application-processor bring-up");
	int smpCpus = arch::smpInit();
	okEnd();
	Console::write("       SMP: ");
	Console::write(smpCpus);
	Console::writeLine(" CPUs online");
	// Now that the online-CPU count is known, expose it via /sys/devices/system/cpu/cpuN so
	// sysfs-based tools (htop, nproc, lscpu) report every core.
	root->populateSysCpu(smpCpus);

	Scheduler::start();

	for (;;) arch::halt_or_hlt();   // unreachable
}

void Kernel::loop() {}

}
;
