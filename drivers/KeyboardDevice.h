/*
 * KeyboardDevice.h — the keyboard as a character device (/dev/input0), evdev-style.
 *
 * Mirrors how the disk is a driver: the hardware path (PS/2 IRQ -> inputFeedScancode,
 * arch) feeds raw scancodes here; this machine-independent device decodes PS/2 set-1
 * make/break codes into key events and queues them, and a process reads them via
 * open("/dev/input0")/read(). Each event is a 2-byte record [code][down]: `code` is a
 * normalised scancode (bit7 = extended/0xE0 key, bits0-6 = the set-1 scancode), `down`
 * is 1 on press and 0 on release. The consumer (e.g. Doom) maps code -> its own keys and
 * tracks the exact held state — no key-up guessing.
 *
 * Pure logic (no hardware) -> host-testable. The PS/2 port + IRQ stay in arch.
 */
#pragma once
#include "CharDevice.h"

namespace kernel {

class KeyboardDevice: public CharDevice {
	static const int CAP = 256;     // ring bytes (128 events)
	unsigned char m_ring[CAP];
	int m_head, m_tail;
	bool m_ext;                     // a 0xE0 extended-key prefix was just seen
	void pushEvent(unsigned char code, unsigned char down);

public:
	KeyboardDevice();

	// Feed one PS/2 set-1 scancode (make, break with bit 0x80, or the 0xE0 prefix).
	void feed(unsigned char sc);

	// CharDevice: read dequeues whole 2-byte events (0 when none — naturally non-blocking).
	int read(unsigned off, void* buf, unsigned n);
	int write(unsigned off, const void* buf, unsigned n);   // unsupported
	int ioctl(unsigned cmd, void* arg);                     // unsupported
	int mmapInfo(uint64_t* physOut, unsigned* lenOut);      // unsupported
	// poll(): readable when an event is queued (so the terminal emulator's poll loop
	// doesn't spin reading an empty device).
	short pollReady(short events) {
		return (short) (((events & POLLIN) && m_head != m_tail) ? POLLIN : 0);
	}
};

// The keyboard sink the arch IRQ path feeds (set once at boot). kbdFeed is a no-op until
// a device is registered, so early scancodes are simply dropped.
void kbdRegister(KeyboardDevice* k);
void kbdFeed(unsigned char sc);

}  // namespace kernel
