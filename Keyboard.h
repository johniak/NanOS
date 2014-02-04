#pragma once

#include "Interrupt.h"
namespace kernel{

#define KERNEL_PAGEDIR        0x0
#define KERNEL_PAGETAB        0x1000
#define KERNEL_GDT            0x2000
#define KERNEL_IDT            0x2500
#define PIC1 0x20
#define PIC2 0xA0
#define ICW1 0x11
#define ICW4 0x01

#define KBD_SPECIAL 200
#define ENTER 10
#define F1 201
#define F2 202
#define F3 203
#define F4 204
#define F5 205
#define F6 206
#define F7 207
#define F8 208
#define F9 209
#define F10 210
#define F11 211
#define F12 212
#define PAUSE 213

class Keyboard{
public:
	void initialize();

};

static void kb_handler(Registers reg);
}
