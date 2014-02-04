#include "Kernel.h"
#include "Console.h"
#include "Idt.h"
#include "Keyboard.h"
namespace kernel{

		void Kernel::start(){
			Console::clearScreen();
			Console::writeLine("NanoOS initialize...");
			Idt idt= Idt();
			idt.initialize();
			Keyboard keyboard = Keyboard();
			keyboard.initialize();
			while(1){
				this->loop();
			}
		} 

		int index=0;
		void Kernel::loop(){
				//Console::writeLine("test");
				//Console::write("Johniak test ");
				//Console::writeLine(index++);
		}	

};
