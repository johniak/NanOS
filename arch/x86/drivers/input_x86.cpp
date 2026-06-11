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
#include "Scheduler.h"
#include "WaitQueue.h"          // console readers park here (multi-waiter, event-driven)
#include "Signal.h"             // SIGINT / SIGQUIT numbers
#include "SignalDispatch.h"     // kernel::consoleSignal / hasPendingSignalCurrent
#include "Syscall.h"            // kernel::EAGAIN (O_NONBLOCK no-data return)
#include "KeyboardDevice.h"     // kernel::kbdFeed (/dev/input0 key events)

namespace {
kernel::LineDiscipline g_line;
kernel::KeyDecoder g_decoder;
int g_raw = 0;
kernel::WaitQueue g_inputWq;       // tasks blocked in inputRead (>=1; the keyboard IRQ wakes all)

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
	// Always feed the /dev/input0 key-event device (evdev-style: it coexists with the
	// console). It decodes make/break itself, so a game gets exact key down/up.
	kernel::kbdFeed(sc);
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
			// Cooked-mode control keys generate signals to the foreground process,
			// like a Unix tty: Ctrl+C -> SIGINT, Ctrl+\ -> SIGQUIT, Ctrl+Z -> SIGTSTP.
			if (ev == 0x03) { kernel::consoleSignal(SIGINT);  return; }
			if (ev == 0x1C) { kernel::consoleSignal(SIGQUIT); return; }
			if (ev == 0x1A) { kernel::consoleSignal(SIGTSTP); return; }
			if (ev < 256) {
				g_line.push((char) ev, echoChar);
			} else {                   // arrow -> ANSI escape bytes into the canonical line,
				g_line.push(0x1B, echoChar);             // exactly as a Unix tty delivers them
				g_line.push('[', echoChar);              // to a program reading cooked input
				g_line.push(ev == kernel::KEY_UP    ? 'A' :
				            ev == kernel::KEY_DOWN  ? 'B' :
				            ev == kernel::KEY_RIGHT ? 'C' : 'D', echoChar);
			}
		});
	}
	// Wake blocked console readers once a read is satisfiable (they re-test their condition).
	if ((g_raw && !rawEmpty()) || (!g_raw && g_line.lineReady()))
		kernel::Scheduler::wakeAll(&g_inputWq);
}

int inputRead(char* buf, unsigned n, int nonblock) {
	if (g_raw) {
		if (nonblock && rawEmpty()) {              // O_NONBLOCK: never block, no data now
			return -EAGAIN;
		}
		while (rawEmpty()) {                       // block until a byte arrives
			kernel::Scheduler::sleepOn(&g_inputWq);    // deschedule; keyboard IRQ wakes us
			if (kernel::hasPendingSignalCurrent())     // woken by a signal, not input
				return -kernel::ERESTARTSYS;       // restart or -> EINTR, decided at delivery
		}
		unsigned i = 0;
		while (i < n && !rawEmpty())
			buf[i++] = (char) rawPop();
		return (int) i;
	}
	if (nonblock && !g_line.lineReady()) {         // O_NONBLOCK cooked: no full line yet
		return -EAGAIN;
	}
	while (!g_line.lineReady()) {                  // block until a full line is ready
		kernel::Scheduler::sleepOn(&g_inputWq);
		if (kernel::hasPendingSignalCurrent())     // woken by a signal, not a full line
			return -kernel::ERESTARTSYS;           // restart or -> EINTR, decided at delivery
	}
	return g_line.takeLine(buf, (int) n);
}

void inputSetRaw(int raw) {
	g_raw = raw;
	g_rawHead = g_rawTail = 0;       // drop buffered input on a mode switch
	g_line = kernel::LineDiscipline();
	g_decoder = kernel::KeyDecoder();
}

bool inputReady() {
	// Same condition inputRead would block on: raw mode needs a buffered byte, cooked mode a
	// committed line. (Mirrors the wake test in inputFeedScancode.)
	return g_raw ? !rawEmpty() : g_line.lineReady();
}

}  // namespace arch
