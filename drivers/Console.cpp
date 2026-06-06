#include "Console.h"
#include "IOPort.h"
#include "string.h"
namespace kernel {

unsigned short Console::cursorX = 0;
unsigned short Console::cursorY = 0;
volatile unsigned short *Console::videoram = (unsigned short *) 0xB8000;
unsigned char attributeByte = (0 << 4) | (15 & 0x0F);
unsigned short blank = ' ' | (attributeByte << 8);
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
		attributeByte, ' ', attributeByte, ' ', attributeByte, ' ',
		attributeByte, ' ', attributeByte };
void Console::scroll() {

	if (cursorY >= 25) {
		int i;
//		for (i = 0 * 80; i < 24 * 80; i++) {
//			videoram[i] = videoram[i + 80];
//		}
		memcpy((void*) videoram, ((void*) videoram) + 160, 24 * 80 * 2);
//		for (i = 24 * 80; i < 25 * 80; i++) {
//			videoram[i] = blank;
//		}
		memcpy((void*) (videoram + (24 * 80)), clearline, 160);
		cursorY = 24;
	}
}
void Console::goToXY(unsigned short x, unsigned short y) {
	cursorX = x;
	cursorY = y;
	moveCursor();
}

void Console::write(char c) {
	unsigned char backColour = 0;
	unsigned char foreColour = 15;

	unsigned char attributeByte = (backColour << 4) | (foreColour & 0x0F);
	unsigned short attribute = attributeByte << 8;
	unsigned short *location;

	if (c == 0x08 && cursorX) {
		cursorX--;
	}

	else if (c == 0x09) {
		cursorX = (cursorX + 8) & ~(8 - 1);
	} else if (c == '\r') {
		cursorX = 0;
	} else if (c == '\n') {
		cursorX = 0;
		cursorY++;
	} else if (c >= ' ') {
		location = ((unsigned short *) videoram) + (cursorY * 80 + cursorX);
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
void Console::write(const char* text) {
	int length = strlen(text);
	for (int i = 0; i < length; i++) {
		write(text[i]);
	}
}
void Console::write(int d) {
	write(itoa(d, 10));
}

void Console::writeHex(int hex) {
	char * ss = itoa(hex, 16);
	if (ss[1] == 0) {
		ss[1] = ss[0];
		ss[0] = '0';
	}
	write(ss);
}

void Console::writeLine(char line) {
	write(line);
	write('\n');
}
void Console::writeLine(const char* line) {
	write(line);
	write('\n');
}
void Console::writeLine(int line) {
	write(line);
	write('\n');
}
void Console::moveCursor() {
	unsigned short cursorLocation = cursorY * 80 + cursorX;
	IOPort::outb(0x3D4, 14); // Tell the VGA board we are setting the high cursor byte.
	IOPort::outb(0x3D5, cursorLocation >> 8); // Send the high cursor byte.
	IOPort::outb(0x3D4, 15); // Tell the VGA board we are setting the low cursor byte.
	IOPort::outb(0x3D5, cursorLocation);      // Send the low cursor byte.
}
void Console::clearScreen() {
	unsigned char attributeByte = (0 << 4) | (15 & 0x0F);
	unsigned short blank = ' ' | (attributeByte << 8);
	for (int i = 0; i < 80 * 25; i++) {
		videoram[i] = blank;
	}
	cursorX = 0;
	cursorY = 0;
	moveCursor();
}
char *Console::itoa(int value, int base) {
#define INT_DIGITS 19
	static char result[32] = { 0 };
	// check that the base if valid
	if (base < 2 || base > 36) {
		*result = '\0';
		return result;
	}

	char* ptr = result, *ptr1 = result, tmp_char;
	int tmp_value;

	do {
		tmp_value = value;
		value /= base;
		*ptr++ =
				"zyxwvutsrqponmlkjihgfedcba9876543210123456789abcdefghijklmnopqrstuvwxyz"[35
						+ (tmp_value - value * base)];
	} while (value);

	// Apply negative sign
	if (tmp_value < 0)
		*ptr++ = '-';
	*ptr-- = '\0';
	while (ptr1 < ptr) {
		tmp_char = *ptr;
		*ptr-- = *ptr1;
		*ptr1++ = tmp_char;
	}
	return result;

}
}
