/*
 * input_x86.cpp — x86 implementation of <arch/input.h>.
 *
 * Owns the console input policy for both modes:
 *  - cooked: scancodes -> KeyDecoder -> LineDiscipline (line editing + echo); read
 *    returns a whole line.
 *  - raw: scancodes -> KeyDecoder -> a raw byte ring (arrows as ESC '[' A/B/C/D, no
 *    echo); read returns whatever bytes are buffered. A shell drives its own editor.
 * The keyboard IRQ (Keyboard.cpp) just forwards scancodes to inputFeedScancode().
 */
#include <arch/input.h>
#include <arch/cpu.h>
#include "LineDiscipline.h"
#include "KeyDecoder.h"
#include "Console.h"

namespace {
kernel::LineDiscipline g_line;
kernel::KeyDecoder g_decoder;
int g_raw = 0;

// Raw-mode byte ring (filled in IRQ context, drained by inputRead).
const int RAWCAP = 256;
volatile unsigned char g_rawbuf[RAWCAP];
volatile int g_rawHead = 0;
volatile int g_rawTail = 0;

void rawPush(unsigned char b) {
	int next = (g_rawHead + 1) % RAWCAP;
	if (next != g_rawTail) {        // drop on overflow
		g_rawbuf[g_rawHead] = b;
		g_rawHead = next;
	}
}
bool rawEmpty() { return g_rawHead == g_rawTail; }
unsigned char rawPop() {
	unsigned char b = g_rawbuf[g_rawTail];
	g_rawTail = (g_rawTail + 1) % RAWCAP;
	return b;
}

void echoChar(char c) { kernel::Console::write(c); }
}

namespace arch {

// Called from the keyboard IRQ for every scancode byte.
void inputFeedScancode(unsigned char sc) {
	if (g_raw) {
		g_decoder.feed(sc, [](int ev) {
			if (ev < 256) {
				rawPush((unsigned char) ev);
			} else {                  // arrow -> ANSI escape sequence
				rawPush(0x1B);
				rawPush('[');
				rawPush(ev == kernel::KEY_UP    ? 'A' :
				        ev == kernel::KEY_DOWN  ? 'B' :
				        ev == kernel::KEY_RIGHT ? 'C' : 'D');
			}
		});
	} else {
		g_decoder.feed(sc, [](int ev) {
			if (ev < 256)              // cooked: arrows ignored
				g_line.push((char) ev, echoChar);
		});
	}
}

int inputRead(char* buf, unsigned n) {
	if (g_raw) {
		while (rawEmpty()) {
			arch::cpuEnableInterrupts();
			arch::cpuHalt();
		}
		unsigned i = 0;
		while (i < n && !rawEmpty())
			buf[i++] = (char) rawPop();
		return (int) i;
	}
	while (!g_line.lineReady()) {
		arch::cpuEnableInterrupts();
		arch::cpuHalt();
	}
	return g_line.takeLine(buf, (int) n);
}

void inputSetRaw(int raw) {
	g_raw = raw;
	g_rawHead = g_rawTail = 0;       // drop buffered input on a mode switch
	g_line = kernel::LineDiscipline();
	g_decoder = kernel::KeyDecoder();
}

}  // namespace arch
