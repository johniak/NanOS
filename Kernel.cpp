#include "Kernel.h"
#include "Console.h"
namespace kernel{

		void Kernel::start(){
			Console::clearScreen();
			Console::writeLine("NanoOS initialize...");
			while(1){
				this->loop();
			}
		} 

		int index=0;
		void Kernel::loop(){
				Console::write("Johniak test ");
				Console::writeLine(index++);
		}	

};
