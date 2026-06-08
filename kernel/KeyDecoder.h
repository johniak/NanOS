/*
 * KeyDecoder.h — PS/2 scancode -> semantic key events (machine-independent).
 *
 * Drives raw-mode console input. feed() turns a stream of scancodes into events:
 * a byte (0..255: printable ASCII, '\n' for Enter, 0x08 for Backspace, or a control
 * code 0x01..0x1A when Ctrl is held with a letter, e.g. Ctrl+C -> 0x03) or one of
 * the KEY_* arrow codes (>255). Handles the 0xE0 extended prefix, tracks the Ctrl
 * modifier, and ignores key releases. The cooked path reuses scancodeAscii().
 * Host-testable (template emit).
 */
#ifndef KEYDECODER_H_
#define KEYDECODER_H_

namespace kernel {

enum {
	KEY_UP = 0x101,
	KEY_DOWN = 0x102,
	KEY_RIGHT = 0x103,
	KEY_LEFT = 0x104,
};

class KeyDecoder {
	bool m_ext;    // a 0xE0 extended-key prefix was just seen
	bool m_ctrl;   // a Ctrl key (left 0x1D or extended-right 0xE0 0x1D) is held
public:
	KeyDecoder() : m_ext(false), m_ctrl(false) {}

	// US scancode (set 1) -> ASCII, or 0 for non-text keys. '\b' for Backspace,
	// '\n' for Enter. Defined in KeyDecoder.cpp.
	static char scancodeAscii(unsigned char sc);

	// Feed one scancode; emit(int) is called once per produced event. emit is a
	// forwarding reference so a stateful sink (e.g. a recorder) accumulates.
	template <class Emit>
	void feed(unsigned char sc, Emit&& emit) {
		if (sc == 0xE0) {            // extended-key prefix
			m_ext = true;
			return;
		}
		if (m_ext) {
			m_ext = false;
			if (sc == 0x1D) { m_ctrl = true;  return; }   // right Ctrl press
			if (sc == 0x9D) { m_ctrl = false; return; }   // right Ctrl release
			if (sc & 0x80)
				return;              // release of an extended key
			switch (sc) {
			case 0x48: emit(KEY_UP); break;
			case 0x50: emit(KEY_DOWN); break;
			case 0x4D: emit(KEY_RIGHT); break;
			case 0x4B: emit(KEY_LEFT); break;
			default: break;
			}
			return;
		}
		if (sc == 0x1D) { m_ctrl = true;  return; }   // left Ctrl press
		if (sc == 0x9D) { m_ctrl = false; return; }   // left Ctrl release
		if (sc & 0x80)
			return;                  // key release
		char a = scancodeAscii(sc);
		if (a == 0)
			return;
		if (m_ctrl && ((a >= 'a' && a <= 'z') || a == '\\')) {
			emit(a & 0x1F);   // Ctrl+letter / Ctrl+\ -> control code (Ctrl+C=0x03, Ctrl+\=0x1C)
			return;
		}
		emit((int) (unsigned char) (a == '\b' ? 0x08 : a));
	}
};

}  // namespace kernel

#endif /* KEYDECODER_H_ */
