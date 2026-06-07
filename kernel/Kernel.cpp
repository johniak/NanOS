#include <stdint.h>
#include "Kernel.h"
#include "Console.h"
#include "BlockDevice.h"
#include "DeviceManager.h"
#include "Vfs.h"
#include "Ext2Filesystem.h"
#include "Ext4Filesystem.h"
#include "SynthFs.h"
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

	// Launch the interactive shell. It reads commands from the keyboard and spawns
	// /bin/<cmd>.nxe programs (cat, ls, ...) until the user types `exit`.
	// Programs live on the system volume: /disks/main/bin (the disk is mounted under
	// /disks/main, not at "/").
	Console::writeLine("--- starting /disks/main/bin/nsh.nxe ---");
	int rc = execProgram(vfs, "/disks/main/bin/nsh.nxe");
	Console::write("nsh exited with code ");
	Console::writeLine(rc);

	// Multitasking is experimental/incomplete (no /init.bin, debug-printing
	// scheduler). Disabled for now so the kernel runs a clean main loop.
	// MultiTasking mt = MultiTasking(ext2Filesystem);
	// mt.exec("/init.bin");
	// mt.start();
	//init_timer(50);
//	for (int i = 0; i < 30; i++) {
//		Console::writeLine(i);
//	}
//	String str =String("repeat");
//	Console::writeLine(S"yolo"+S"rower"+10);
//	String test = "Ala ma ma kota";
//	Console::writeLine(S"indexOf: "+test.substring(test.indexOf("ma",5)));
//	List<String> strs= test.split(' ');
//	for(int i=0;i<strs.getCount();i++){
//		//Console::writeLine(strs[i]);
//		Console::writeLine(S""+strs[i]);
//	}
//	for (int i = 0; i < 512; i++) {
//		int s = (int) buf[i];
//		s &= 0x000000FF;
//		Console::writeHex(s);
//		if ((i + 1) % 27 != 0)
//			Console::write(" ");
//	}

	while (1) {
		this->loop();
	}
}

int index = 0;
void Kernel::loop() {
	//Console::writeLine("test");
	//Console::write("Johniak test ");
	// Idle: counter print disabled to keep the screen quiet.
	index++;
	//Console::writeLine(index);
	//if (index % 100000000==0){
	//	Console::writeLine(index);
	//}
}

}
;
