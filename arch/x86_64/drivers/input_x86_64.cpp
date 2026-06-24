/*
 * input_x86_64.cpp — x86-64 implementation of <arch/input.h>.
 *
 * With virtual terminals (kernel/vt), the console input state (line discipline, raw ring, wait
 * queue, termios) lives per-VT in VtConsole, NOT in this file. This layer now does two things:
 *   1. intercept Ctrl+Alt+Fn (a small modifier matcher independent of the per-VT KeyDecoder, which
 *      does not track Alt) and ask VtManager to switch console — the scancode is consumed;
 *   2. route every other scancode to the ACTIVE VtConsole (and to /dev/input0 for that VT).
 *
 * The PS/2 IRQ1 path feeds inputFeedScancode the same way on both arches (the loadable keyboard
 * kext calls knx_feed_scancode -> arch::inputFeedScancode). inputRead/inputSetRaw/inputReady are
 * thin forwarders to the active VT (the console fd's per-VT binding is applied by the syscall
 * layer; the bare arch contract acts on the active console).
 */
#include <arch/input.h>
#include "vt/VtManager.h"
#include "KeyboardDevice.h"     // kernel::kbdFeed (/dev/input0 key events)

namespace {
// Minimal Ctrl+Alt+Fn matcher. Independent of the per-VT KeyDecoder (which tracks Ctrl/Shift for
// control codes but not Alt). Tracks make/break of Ctrl (0x1D) and Alt (0x38), incl. the 0xE0
// extended (right) variants. Returns 1..7 when this scancode COMPLETES Ctrl+Alt+F1..F7, else 0.
bool g_ctrl = false, g_alt = false;
bool g_ext = false;
int vtSwitchMatch(unsigned char sc) {
	if (sc == 0xE0) { g_ext = true; return 0; }
	bool brk = (sc & 0x80) != 0;
	unsigned char code = sc & 0x7F;
	if (g_ext) {
		g_ext = false;
		if (code == 0x38) g_alt = !brk;       // right Alt (AltGr)
		if (code == 0x1D) g_ctrl = !brk;       // right Ctrl
		return 0;
	}
	if (code == 0x38) { g_alt = !brk; return 0; }    // left Alt
	if (code == 0x1D) { g_ctrl = !brk; return 0; }    // left Ctrl
	if (!brk && g_ctrl && g_alt && code >= 0x3B && code <= 0x41)
		return (int) (code - 0x3B) + 1;               // F1..F7 -> 1..7
	return 0;
}
}

namespace arch {

// Called from the keyboard IRQ for every scancode byte.
void inputFeedScancode(unsigned char sc) {
	int target = vtSwitchMatch(sc);
	if (target) {                              // Ctrl+Alt+Fn: switch console, consume the key
		if (kernel::g_vtmgr) kernel::g_vtmgr->requestSwitch(target);
		return;
	}
	if (!kernel::g_vtmgr) return;              // pre-VT window (keyboard IRQ before VtManager): drop
	// Route by the ACTIVE console's mode: a graphics VT (nwm on F7) consumes raw key events via
	// /dev/input0 (evdev); a text VT runs the per-VT line discipline. So a backgrounded graphics
	// app gets no keys until its console is switched in, and text VTs never leak to evdev.
	if (kernel::g_vtmgr->activeVt()->mode() == KD_GRAPHICS)
		kernel::kbdFeed(sc);                   // evdev for the graphics owner
	else
		kernel::g_vtmgr->feedActive(sc);       // text VT: line discipline (+ echo, under the VT lock)
}

int inputRead(char* buf, unsigned n, int nonblock) {
	if (!kernel::g_vtmgr) return 0;
	return kernel::g_vtmgr->activeVt()->read(buf, n, nonblock != 0);
}

void inputSetRaw(int raw) {
	if (kernel::g_vtmgr) kernel::g_vtmgr->activeVt()->setRaw(raw != 0);
}

bool inputReady() {
	return kernel::g_vtmgr ? kernel::g_vtmgr->activeVt()->inputReady() : false;
}

}  // namespace arch
