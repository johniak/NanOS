#include "Kernel.h"
#include "Console.h"
#include "Idt.h"
#include "Keyboard.h"
#include "Hdd.h"
#include "Ext2Filesystem.h"
#include "List.h"
#include "String.h"
char buf[1024];



namespace kernel {



void Kernel::start() {
	Console::clearScreen();
	Console::writeLine("NanoOS initialize...");



	Idt idt = Idt();
	idt.initialize();
	Keyboard keyboard = Keyboard();
	keyboard.initialize();
	Console::writeLine("");
//	char* bb = buf;

	Ext2Filesystem ext2Filesystem=Ext2Filesystem();
	ext2Filesystem.initialize(2048);
	String str =String("repeat");
	Console::writeLine(S"yolo"+S"rower"+10);
	String test = "Ala ma ma kota";
	Console::writeLine(S"indexOf: "+test.substring(test.indexOf("ma",5)));
	List<String> strs= test.split(' ');
	for(int i=0;i<strs.getCount();i++){
		//Console::writeLine(strs[i]);
		Console::writeLine(S""+strs[i]);
	}
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
	//Console::writeLine(index++);
}

}
;
