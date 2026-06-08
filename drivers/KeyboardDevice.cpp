/*
 * KeyboardDevice.cpp — implementation of the /dev/input0 key-event device. See header.
 */
#include "KeyboardDevice.h"

namespace kernel {

KeyboardDevice::KeyboardDevice() : m_head(0), m_tail(0), m_ext(false) {
	for (int i = 0; i < CAP; i++)
		m_ring[i] = 0;
}

void KeyboardDevice::pushEvent(unsigned char code, unsigned char down) {
	// Need 2 free bytes; on overflow drop the oldest event (2 bytes) to keep framing.
	if (((m_head + 1) % CAP) == m_tail || ((m_head + 2) % CAP) == m_tail)
		m_tail = (m_tail + 2) % CAP;
	m_ring[m_head] = code;       m_head = (m_head + 1) % CAP;
	m_ring[m_head] = down;       m_head = (m_head + 1) % CAP;
}

void KeyboardDevice::feed(unsigned char sc) {
	if (sc == 0xE0) {                 // extended-key prefix: the next byte is the key
		m_ext = true;
		return;
	}
	unsigned char down = (sc & 0x80) ? 0 : 1;                 // break bit => release
	unsigned char code = (unsigned char) ((m_ext ? 0x80 : 0) | (sc & 0x7F));
	m_ext = false;
	pushEvent(code, down);
}

int KeyboardDevice::read(unsigned /*off*/, void* buf, unsigned n) {
	unsigned char* out = (unsigned char*) buf;
	unsigned i = 0;
	while (i + 2 <= n && m_tail != m_head) {     // whole events only
		out[i++] = m_ring[m_tail]; m_tail = (m_tail + 1) % CAP;
		out[i++] = m_ring[m_tail]; m_tail = (m_tail + 1) % CAP;
	}
	return (int) i;
}

int KeyboardDevice::write(unsigned, const void*, unsigned) { return -30; }   // -EROFS
int KeyboardDevice::ioctl(unsigned, void*) { return -22; }                   // -EINVAL
int KeyboardDevice::mmapInfo(unsigned*, unsigned*) { return -22; }           // -EINVAL

namespace { KeyboardDevice* g_kbd = 0; }
void kbdRegister(KeyboardDevice* k) { g_kbd = k; }
void kbdFeed(unsigned char sc) { if (g_kbd) g_kbd->feed(sc); }

}  // namespace kernel
