/*
 * Keyboard.cpp
 *
 *  Created on: Feb 3, 2014
 *      Author: johniak
 */
#include "Keyboard.h"
#include "Idt.h"
#include "Console.h"
#include "LineDiscipline.h"

namespace kernel {
// The system console line buffer (cooked mode). The IRQ1 handler feeds it; the
// blocking read(0) syscall drains it via <arch/input.h> (input_x86.cpp).
LineDiscipline g_lineDiscipline;

static char scancode_ascii[0x100] = {
KBD_SPECIAL, KBD_SPECIAL, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-',
		'=',
		'\b', KBD_SPECIAL, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o',
		'p', '[', ']', '\n', KBD_SPECIAL, 'a', 's', 'd', 'f', 'g', 'h', 'j',
		'k', 'l', ';', '\'', KBD_SPECIAL, '\\', '<', 'z', 'x', 'c', 'v', 'b',
		'n', 'm', ',', '.', '/', KBD_SPECIAL, KBD_SPECIAL, KBD_SPECIAL, ' ',
		KBD_SPECIAL, F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12, PAUSE,
		KBD_SPECIAL, KBD_SPECIAL, };
static void echoChar(char c) { Console::write(c); }

static void kb_handler(Registers* reg) {
	unsigned char sc = (unsigned char) IOPort::inb(0x60);
	if (sc & 0x80)
		return;                         // key release — ignore
	char key = scancode_ascii[sc];
	if (key == KBD_SPECIAL)
		return;                         // non-text key (Esc/Tab/F-keys/...)
	g_lineDiscipline.push(key, echoChar);
}
void Keyboard::initialize() {
	Interrupt::registerInterruptHandler(IRQ1, kb_handler);
}
}

