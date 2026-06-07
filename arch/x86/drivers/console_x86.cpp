/*
 * console_x86.cpp — x86 PC VGA text-mode implementation of <arch/console.h>.
 *
 * 80x25 text buffer at 0xB8000, hardware cursor via VGA ports 0x3D4/0x3D5.
 * This is the machine-dependent console sink; MI Console formatting calls these.
 */
#include <arch/console.h>
#include "IOPort.h"
#include <string.h>

namespace {

unsigned short cursorX = 0;
unsigned short cursorY = 0;
volatile unsigned short* videoram = (unsigned short*) 0xB8000;

const unsigned char attributeByte = (0 << 4) | (15 & 0x0F);
// One blank line worth of {char, attribute} pairs, used to clear the bottom row.
char clearline[] = { ' ', attributeByte, ' ', attributeByte, ' ', attributeByte,
		' ', attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte };

void scroll() {
	if (cursorY >= 25) {
		memcpy((void*) videoram, ((void*) videoram) + 160, 24 * 80 * 2);
		memcpy((void*) (videoram + (24 * 80)), clearline, 160);
		cursorY = 24;
	}
}

void moveCursor() {
	unsigned short cursorLocation = cursorY * 80 + cursorX;
	kernel::IOPort::outb(0x3D4, 14);
	kernel::IOPort::outb(0x3D5, cursorLocation >> 8);
	kernel::IOPort::outb(0x3D4, 15);
	kernel::IOPort::outb(0x3D5, cursorLocation);
}

}  // namespace

namespace arch {

void consolePutChar(char c) {
	unsigned char backColour = 0;
	unsigned char foreColour = 15;
	unsigned char attributeByte = (backColour << 4) | (foreColour & 0x0F);
	unsigned short attribute = attributeByte << 8;
	unsigned short* location;

	if (c == 0x08 && cursorX) {
		cursorX--;
	} else if (c == 0x09) {
		cursorX = (cursorX + 8) & ~(8 - 1);
	} else if (c == '\r') {
		cursorX = 0;
	} else if (c == '\n') {
		cursorX = 0;
		cursorY++;
	} else if (c >= ' ') {
		location = ((unsigned short*) videoram) + (cursorY * 80 + cursorX);
		*location = c | attribute;
		cursorX++;
	}
	if (cursorX >= 80) {
		cursorX = 0;
		cursorY++;
	}
	scroll();
	moveCursor();
}

void consoleClear() {
	unsigned char attributeByte = (0 << 4) | (15 & 0x0F);
	unsigned short blank = ' ' | (attributeByte << 8);
	for (int i = 0; i < 80 * 25; i++)
		videoram[i] = blank;
	cursorX = 0;
	cursorY = 0;
	moveCursor();
}

void consoleSetCursor(unsigned x, unsigned y) {
	cursorX = x;
	cursorY = y;
	moveCursor();
}

void consoleInit() {
	consoleClear();
}

}  // namespace arch
