#include <stdint.h>
#include "Kernel.h"
#include "Console.h"
#include "Gdt.h"
#include "Idt.h"
#include "Interrupt.h"
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
#include "MultibootInfo.h"
#include "MultibootMmap.h"
#include "FrameAllocator.h"
#include "AddressSpace.h"
#include "Paging.h"
#include "PagingControl.h"
char buf[1024];

// Set by arch/loader.s from ebx: physical pointer to the Multiboot info struct.
extern "C" unsigned mbd;
// Linker symbol marking the end of the kernel image (linker.ld).
extern char end;

void interrupt3(kernel::Registers* regs) {
	//asm("int $3");
	//while (true) {
	kernel::Console::writeLine("Hehehehd");
	//}
}
namespace kernel {

// Issue a Linux-style syscall via int 0x80 (nr in eax, args in ebx/ecx/edx).
static int sys3(int nr, int a, int b, int c) {
	int ret;
	asm volatile("int $0x80" : "=a"(ret) : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory");
	return ret;
}

// PagingEnv hooks: physical memory is identity-mapped, so phys==virt.
// (Types must match the function-pointer fields exactly: uint32_t here is the
// toolchain's `long unsigned int`, not `unsigned int`.)
static uint32_t kAllocFrame(void*) { return g_frames.alloc(); }
static void kFreeFrame(void*, uint32_t pa) { g_frames.free(pa); }
static void* kPhysToVirt(void*, uint32_t pa) { return (void*) pa; }

// Mark each usable mmap region as free in the frame allocator.
static void freeUsableRegion(void* fa, uint64_t base, uint64_t len, uint32_t type) {
	if (type == MMAP_TYPE_AVAILABLE)
		((FrameAllocator*) fa)->markRangeFree((uint32_t) base, (uint32_t) len);
}

// Build the physical frame allocator from the Multiboot memory map, construct
// the kernel address space identity-mapping all RAM, and enable paging. Still
// ring 0, single program: every region the kernel/init.nxe touch maps 1:1.
void Kernel::initPaging() {
	MultibootInfo* mbi = (MultibootInfo*) mbd;
	unsigned top = highestUsableAddr(mbi);
	if (top == 0)
		top = 0x8000000;   // fallback: 128 MiB (QEMU default) if no memory info

	g_frames.init(top);
	parseMmap(mbi, &g_frames, freeUsableRegion);

	// Re-reserve the windows the frame pool must never hand out.
	g_frames.markRangeUsed(0, 0x100000);                            // low mem + VGA
	g_frames.markRangeUsed(0x100000, (unsigned) &end - 0x100000);   // kernel image
	g_frames.markRangeUsed(0x400000, 0x100000);                     // user window
	unsigned heapBase = 0x75BCD15 & PAGE_MASK;                      // bump heap
	g_frames.markRangeUsed(heapBase, top - heapBase);

	// The directory + page tables come from the frame pool (below the heap); the
	// AddressSpace object itself comes from the already-reserved bump heap.
	static AddressSpace* kspace = 0;
	PagingEnv env = { kAllocFrame, kFreeFrame, kPhysToVirt, 0 };
	kspace = new AddressSpace(env);
	kspace->mapRange(0, 0, top, PTE_PRESENT | PTE_RW);   // identity-map all RAM

	asm volatile("cli");
	loadCr3(kspace->directoryPhys());
	enablePaging();
	asm volatile("sti");
	Console::writeLine("paging enabled");
}

void Kernel::start() {
	Console::clearScreen();
	Console::writeLine("NanoOS initialize...");

	// Install our own GDT first: the IDT gates use code selector 0x08, which
	// is only valid once we control the GDT layout (bootloaders differ).
	Gdt gdt = Gdt();
	gdt.initialize();

	Idt idt = Idt();
	idt.initialize();
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

	// Install the Linux-style syscall interface (int 0x80) over the VFS.
	installSyscalls(vfs);

	// Sanity: one direct int 0x80 call.
	const char* msg = "syscall write OK\n";
	sys3(SYS_write, 1, (int) msg, 17);

	// Load and run the first userspace program (.nxe) via the dynamic loader.
	Console::writeLine("--- exec /bin/init.nxe ---");
	int rc = execProgram(vfs, "/bin/init.nxe");
	Console::write("init.nxe exited with code ");
	Console::writeLine(rc);

	Interrupt::registerInterruptHandler(3, &interrupt3);
	//asm("int $3");
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
