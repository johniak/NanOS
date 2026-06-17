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

// 64-bit-capable hex (the int overload truncates a 64-bit address). Width follows the
// platform: 32 bits on i686 (ILP32), 64 bits on x86_64 (LP64). Prints "0x" then the value
// with no leading zeros (a lone 0 still prints "0x0"). Part of the minimal LP64 fix; the
// full MI sweep is Plan 7.
void Console::writeHex(unsigned long v) {
	write("0x");
	bool started = false;
	for (int shift = (int) (sizeof(unsigned long) * 8) - 4; shift >= 0; shift -= 4) {
		unsigned digit = (unsigned) ((v >> shift) & 0xFUL);
		if (digit != 0 || started || shift == 0) {
			write((char) (digit < 10 ? '0' + digit : 'a' + digit - 10));
			started = true;
		}
	}
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
