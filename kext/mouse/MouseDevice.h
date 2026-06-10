/*
 * MouseDevice.h — a PS/2 mouse exposed as a Linux-style evdev character device.
 *
 * Machine-independent (host-testable): read() yields whole `struct input_event` records and
 * feed() turns raw PS/2 packet bytes into EV_REL/EV_KEY/EV_SYN events. The x86 hardware glue
 * (8042 init + IRQ12) lives in mouse_ps2.cpp. Ships inside mouse.nkext, NOT in the kernel.
 */
#pragma once
#include "CharDevice.h"   // kernel::CharDevice + POLLIN (drivers/)

namespace kext {

// Linux i386 struct input_event (16 bytes): timeval{sec,usec} + type + code + value.
struct InputEvent {
	unsigned tv_sec;
	unsigned tv_usec;
	unsigned short type;
	unsigned short code;
	int value;
};

// evdev event types / codes (subset needed for a 3-button mouse).
enum { EV_SYN = 0, EV_KEY = 1, EV_REL = 2 };
enum { REL_X = 0, REL_Y = 1 };
enum { BTN_LEFT = 0x110, BTN_RIGHT = 0x111, BTN_MIDDLE = 0x112 };
enum { SYN_REPORT = 0 };

class MouseDevice : public kernel::CharDevice {
public:
	MouseDevice();

	// CharDevice — non-blocking evdev semantics (like KeyboardDevice).
	int read(unsigned off, void* buf, unsigned n) override;
	int write(unsigned off, const void* buf, unsigned n) override;
	int ioctl(unsigned cmd, void* arg) override;
	int mmapInfo(unsigned* physOut, unsigned* lenOut) override;
	short pollReady(short events) override;

	// Consume one raw PS/2 byte; `now_us` stamps any events emitted by a completed packet.
	void feed(unsigned char byte, unsigned long long now_us);

private:
	void push(unsigned short type, unsigned short code, int value, unsigned long long now_us);

	static const int CAP = 64;       // event ring capacity
	InputEvent m_ring[CAP];
	int m_head, m_tail;

	unsigned char m_pkt[3];          // PS/2 3-byte packet assembly
	int m_idx;
	int m_buttons;                   // previous button bitmask (for press/release edges)
};

}  // namespace kext
