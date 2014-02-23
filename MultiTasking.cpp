/*
 * MultiTasking.cpp
 *
 *  Created on: Feb 7, 2014
 *      Author: johniak
 */

#include "MultiTasking.h"
namespace kernel {
List<Process> processList = List<Process>();
int actualProcess = 0;
void timer_callbackC(Registers* regs);
void init_timer1(unsigned frequency);
bool wasKernel=false;
int eip;
void timer_callbackC(Registers* regs) {
	if (processList.getCount() > 0) {
		Registers regst=*regs;
	//	memcpy(&(processList[actualProcess].regs),regs,sizeof(Registers));
		processList[actualProcess].regs=regst;
		eip=regs->eip;
		actualProcess = (actualProcess + 1) % processList.getCount();
		//Console::writeLine("Process changed to: ");
		//Console::writeLine(actualProcess);
		//regs=processList[actualProcess].regs;

		//memcpy(regs,&(processList[actualProcess].regs),sizeof(Registers));
		Console::writeLine((int)regs->eip);
		Console::writeLine((int)processList[actualProcess].regs.eip);
			for(int i=0;i<1000000000;i++){

			}
		regs->eip=processList[actualProcess].regs.eip;
	}else{
		Console::writeLine(S "You must start at last one process");
	}
}
void init_timer1(unsigned frequency) {
	// Firstly, register our timer callback.
	kernel::Interrupt::registerInterruptHandler(IRQ0, &timer_callbackC);

	// The value we send to the PIT is the value to divide it's input clock
	// (1193180 Hz) by, to get our required frequency. Important to note is
	// that the divisor must be small enough to fit into 16-bits.
	unsigned divisor = 1193180 / frequency;

	// Send the command byte.
	kernel::IOPort::outb(0x43, 0x36);

	// Divisor has to be sent byte-wise, so split here into upper/lower bytes.
	char l = (char) (divisor & 0xFF);
	char h = (char) ((divisor >> 8) & 0xFF);
	// Send the frequency divisor.
	kernel::IOPort::outb(0x40, l);
	kernel::IOPort::outb(0x40, h);
}

MultiTasking::MultiTasking(Ext2Filesystem ext2fs) {
	this->ext2fs = ext2fs;
	//MultiTasking::processList=List<Process>();
	stackSize = 4096;
	//actualProcess=-1;
}
void MultiTasking::start() {
	Process proc = Process();
	processList.add(proc);
	initTimer(10);
}
void MultiTasking::exec(String filename) {
	char buf[1024];
	int size = ext2fs.readFile("/init.bin", 1024, 0, buf);
	if (size <= 0) {
		Console::writeLine(
				S "Problem with open file: " + filename + S " Error: " + size);
		return;
	}
	Console::writeLine(size);
	Process proc = Process();
	proc.memorySize = size + stackSize;
	proc.pid = processList.getCount();
	void* memory = malloc(proc.memorySize);
	proc.memoryStart = (size_t) memory;
	proc.stackPointer = (size_t) memory + proc.memorySize;
	proc.regs.eip=proc.memoryStart;
	memcpy(memory,(void*)buf,size);
	processList.add(proc);
}
void MultiTasking::initTimer(unsigned frequency) {
	init_timer1(frequency);
}
}

