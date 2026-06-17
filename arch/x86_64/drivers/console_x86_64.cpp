/*
 * console_x86_64.cpp — x86_64 VGA text-mode implementation of <arch/console.h>.
 *
 * 80x25 text buffer at physical 0xB8000 (reachable through the loader's identity map),
 * hardware cursor via VGA ports 0x3D4/0x3D5. A self-contained Plan-2 sink: no framebuffer
 * console yet (consoleActivateFramebuffer is a no-op; the fbcon takeover lands with the
 * graphics work in a later plan). Port I/O uses inline asm directly (this is MD code, so the
 * MI arch-cleanliness guard does not apply here).
 */
#include <arch/console.h>
#include <string.h>

namespace {

unsigned short cursorX = 0;
unsigned short cursorY = 0;
volatile unsigned short* videoram = (volatile unsigned short*) 0xB8000;

const unsigned short ATTR = 0x0F00;   // white on black, in the high byte of each cell

inline void outb(unsigned short port, unsigned char val) {
	__asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

void moveCursor() {
	unsigned short loc = (unsigned short) (cursorY * 80 + cursorX);
	outb(0x3D4, 14);
	outb(0x3D5, (unsigned char) (loc >> 8));
	outb(0x3D4, 15);
	outb(0x3D5, (unsigned char) loc);
}

void scroll() {
	if (cursorY >= 25) {
		memcpy((void*) videoram, (void*) (videoram + 80), 24 * 80 * 2);
		for (int i = 24 * 80; i < 25 * 80; i++)
			((unsigned short*) videoram)[i] = (unsigned short) (ATTR | ' ');
		cursorY = 24;
	}
}

}  // namespace

namespace arch {

void consolePutChar(char c) {
	if (c == 0x08 && cursorX) {
		cursorX--;
	} else if (c == 0x09) {
		cursorX = (unsigned short) ((cursorX + 8) & ~(8 - 1));
	} else if (c == '\r') {
		cursorX = 0;
	} else if (c == '\n') {
		cursorX = 0;
		cursorY++;
	} else if (c >= ' ') {
		videoram[cursorY * 80 + cursorX] = (unsigned short) (ATTR | (unsigned char) c);
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
	const unsigned short blank = (unsigned short) (ATTR | ' ');
	for (int i = 0; i < 80 * 25; i++)
		videoram[i] = blank;
	cursorX = 0;
	cursorY = 0;
	moveCursor();
}

void consoleSetCursor(unsigned x, unsigned y) {
	cursorX = (unsigned short) x;
	cursorY = (unsigned short) y;
	moveCursor();
}

void consoleInit() {
	consoleClear();
}

// Plan 2: VGA text only. The framebuffer (fbcon) takeover lands with the graphics work.
void consoleActivateFramebuffer() {}

void consoleSize(unsigned* cols, unsigned* rows) {
	if (cols) *cols = 80;
	if (rows) *rows = 25;
}

}  // namespace arch
