/*
 * MultiTasking.h
 *
 *  Created on: Feb 7, 2014
 *      Author: johniak
 */
#pragma once
#include "Console.h"
#include "Idt.h"
#include "Interrupt.h"
#include "Ext2Filesystem.h"
#include "List.h"
#ifndef MULTITASKING_H_
#define MULTITASKING_H_

namespace kernel {


class Process{
public:
	Registers regs;
	unsigned pid;
	unsigned memorySize;
	unsigned stackPointer;
	unsigned memoryStart;
};



class MultiTasking {
	Ext2Filesystem ext2fs;
	unsigned stackSize;

public:
	MultiTasking(Ext2Filesystem ext2fs);
	void start();
	void exec(String filename);
	void initTimer(unsigned frequency);
};


}
#endif /* MULTITASKING_H_ */
