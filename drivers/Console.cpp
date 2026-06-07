/*
 * Console.cpp — machine-independent console formatting.
 *
 * Numeric/string/line formatting that drives the arch character sink
 * (<arch/console.h>). No hardware here; the VGA specifics live in
 * arch/x86/drivers/console_x86.cpp.
 */
#include "Console.h"
#include <arch/console.h>
#include "string.h"

namespace kernel {

void Console::write(char c) {
	arch::consolePutChar(c);
}
void Console::write(const char* text) {
	int length = strlen(text);
	for (int i = 0; i < length; i++)
		write(text[i]);
}
void Console::write(int d) {
	write(itoa(d, 10));
}

void Console::writeHex(int hex) {
	char* ss = itoa(hex, 16);
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

void Console::goToXY(unsigned short x, unsigned short y) {
	arch::consoleSetCursor(x, y);
}
void Console::clearScreen() {
	arch::consoleClear();
}

char* Console::itoa(int value, int base) {
	static char result[32] = { 0 };
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
