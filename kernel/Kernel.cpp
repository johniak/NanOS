#include <stdint.h>
#include "Kernel.h"
#include "Console.h"
#include "Keyboard.h"
#include "Hdd.h"
#include "BlockDevice.h"
#include "AtaBlockDevice.h"
#include "DeviceManager.h"
#include "Vfs.h"
#include "Ext2Filesystem.h"
#include "Ext4Filesystem.h"
#include "Syscall.h"
#include "SyscallDispatch.h"
#include "Exec.h"
#include "List.h"
#include "String.h"
#include "MultiTasking.h"
#include <arch/bootinfo.h>
#include <arch/mmu.h>
#include <arch/cpu.h>
#include <arch/syscall.h>
#include "FrameAllocator.h"
char buf[1024];

namespace kernel {

// Mark a usable physical range free in the frame allocator (arch reports only
// usable ranges via <arch/bootinfo.h>).
static void markFree(void* fa, uint64_t base, uint64_t len) {
	((FrameAllocator*) fa)->markRangeFree((uint32_t) base, (uint32_t) len);
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

	// Bring up the CPU descriptor tables + interrupt vectors (arch).
	arch::cpuInit();
	Keyboard keyboard = Keyboard();
	keyboard.initialize();
	Console::writeLine("");

	// Enable paging (identity-mapped) before the storage stack / userspace.
	initPaging();
//	char* bb = buf;
	//kernel::Interrupt::registerInterruptHandler(, &callback3);

	// Storage stack: register the ATA disk as a block device, register the ext2
	// filesystem type, and mount it at "/". All access goes through the VFS.
	AtaBlockDevice* hd0 = new AtaBlockDevice("hd0");
	DeviceManager::registerDevice(hd0);
	Vfs* vfs = new Vfs();
	vfs->registerType(new Ext4FileSystemType());
	vfs->registerType(new Ext2FileSystemType());
	vfs->mount("/", "auto", hd0, 2048);

	// Install the syscall interface over the VFS, then a boot sanity syscall.
	installSyscalls(vfs);
	arch::syscallSelfTest();

	// Load and run the first userspace program (.nxe) via the dynamic loader.
	Console::writeLine("--- exec /bin/init.nxe ---");
	int rc = execProgram(vfs, "/bin/init.nxe");
	Console::write("init.nxe exited with code ");
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
