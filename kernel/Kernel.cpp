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
#include "List.h"
#include "String.h"
#include "MultiTasking.h"
char buf[1024];

void interrupt3(kernel::Registers* regs) {
	//asm("int $3");
	//while (true) {
	kernel::Console::writeLine("Hehehehd");
	//}
}
namespace kernel {

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

	// Demo: list /boot/grub and print grub.cfg through the VFS.
	List<DirEntry> entries;
	if (vfs->readdir("/boot/grub", entries) == 0) {
		Console::writeLine("Contents of /boot/grub:");
		for (int i = 0; i < entries.getCount(); i++)
			Console::writeLine(entries[i].name);
	}
	char cfg[256];
	int n = vfs->read("/boot/grub/grub.cfg", 255, 0, cfg);
	if (n > 0) {
		cfg[n] = 0;
		Console::writeLine("--- /boot/grub/grub.cfg ---");
		Console::write(cfg);
	}

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
