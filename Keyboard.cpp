/*
 * Keyboard.cpp
 *
 *  Created on: Feb 3, 2014
 *      Author: johniak
 */
#include "Keyboard.h"
#include "Idt.h"
#include "Console.h"

namespace kernel {
static char scancode_ascii[0x100] = {
KBD_SPECIAL, KBD_SPECIAL, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-',
		'=',
		KBD_SPECIAL, KBD_SPECIAL, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o',
		'p', '[', ']', '\n', KBD_SPECIAL, 'a', 's', 'd', 'f', 'g', 'h', 'j',
		'k', 'l', ';', '\'', KBD_SPECIAL, '\\', '<', 'z', 'x', 'c', 'v', 'b',
		'n', 'm', ',', '.', '/', KBD_SPECIAL, KBD_SPECIAL, KBD_SPECIAL, ' ',
		KBD_SPECIAL, F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12, PAUSE,
		KBD_SPECIAL, KBD_SPECIAL, };
static void kb_handler(Registers* reg) {
	char key;
	char kbd_scancode;
	kbd_scancode = IOPort::inb(0x60);
	if (kbd_scancode > 0) {
		key = scancode_ascii[(int)kbd_scancode];
		Console::write(key);
	}
}
void Keyboard::initialize() {
	Interrupt::registerInterruptHandler(IRQ1, kb_handler);
}
}

