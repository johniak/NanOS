#include "MouseDevice.h"

namespace kext {

MouseDevice::MouseDevice() : m_head(0), m_tail(0), m_idx(0), m_buttons(0) {}

void MouseDevice::push(unsigned short type, unsigned short code, int value, unsigned long long now_us) {
	int next = (m_head + 1) % CAP;
	if (next == m_tail)
		return;                       // ring full: drop (input is best-effort)
	InputEvent& e = m_ring[m_head];
	e.tv_sec = (unsigned) (now_us / 1000000ull);
	e.tv_usec = (unsigned) (now_us % 1000000ull);
	e.type = type;
	e.code = code;
	e.value = value;
	m_head = next;
}

void MouseDevice::feed(unsigned char byte, unsigned long long now_us) {
	// Byte 0 of every PS/2 packet has bit 3 set; use it to (re)synchronise the stream.
	if (m_idx == 0 && !(byte & 0x08))
		return;
	m_pkt[m_idx++] = byte;
	int plen = m_wheel ? 4 : 3;
	if (m_idx < plen)
		return;
	m_idx = 0;

	unsigned char b0 = m_pkt[0];
	if (b0 & 0xC0)
		return;                       // X/Y overflow bits set: drop the packet (as Linux does)

	// 9-bit signed deltas: magnitude in byte 1/2, sign bit in b0 (bit4 = X, bit5 = Y).
	int dx = (int) m_pkt[1] - ((b0 & 0x10) ? 256 : 0);
	int dy = (int) m_pkt[2] - ((b0 & 0x20) ? 256 : 0);
	if (dx)
		push(EV_REL, REL_X, dx, now_us);
	if (dy)
		push(EV_REL, REL_Y, -dy, now_us);   // PS/2 up is +Y; evdev/screen is +Y down

	int b = b0 & 0x07;                // bit0 left, bit1 right, bit2 middle
	int changed = b ^ m_buttons;
	if (changed & 0x01)
		push(EV_KEY, BTN_LEFT,   (b & 0x01) ? 1 : 0, now_us);
	if (changed & 0x02)
		push(EV_KEY, BTN_RIGHT,  (b & 0x02) ? 1 : 0, now_us);
	if (changed & 0x04)
		push(EV_KEY, BTN_MIDDLE, (b & 0x04) ? 1 : 0, now_us);
	m_buttons = b;

	if (m_wheel) {
		// IntelliMouse 4th byte: scroll Z in the low nibble, two's-complement (−8..+7).
		int z = m_pkt[3] & 0x0f;
		if (z & 0x08) z -= 16;
		if (z) push(EV_REL, REL_WHEEL, z, now_us);   // +1 = wheel forward (Linux convention)
	}

	push(EV_SYN, SYN_REPORT, 0, now_us);   // terminate the report
}

int MouseDevice::read(unsigned, void* buf, unsigned n) {
	unsigned out = 0;
	unsigned char* p = (unsigned char*) buf;
	while (out + sizeof(InputEvent) <= n && m_tail != m_head) {
		const unsigned char* src = (const unsigned char*) &m_ring[m_tail];
		for (unsigned i = 0; i < sizeof(InputEvent); i++)
			p[out + i] = src[i];
		out += sizeof(InputEvent);
		m_tail = (m_tail + 1) % CAP;
	}
	return (int) out;                 // 0 when empty (non-blocking)
}

int MouseDevice::write(unsigned, const void*, unsigned) { return -1; }
int MouseDevice::ioctl(unsigned, void*) { return -1; }
int MouseDevice::mmapInfo(uint64_t*, unsigned*) { return -1; }

short MouseDevice::pollReady(short events) {
	return (short) (((events & POLLIN) && m_head != m_tail) ? POLLIN : 0);
}

}  // namespace kext
