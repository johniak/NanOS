/*
 * KeyDecoder.h — PS/2 scancode -> semantic key events (machine-independent).
 *
 * Drives raw-mode console input. feed() turns a stream of scancodes into events:
 * a byte (0..255: printable ASCII, '\n' for Enter, 0x08 for Backspace, or a control
 * code 0x01..0x1A when Ctrl is held with a letter, e.g. Ctrl+C -> 0x03) or one of
 * the KEY_* arrow codes (>255). Handles the 0xE0 extended prefix, tracks the Ctrl
 * and Shift modifiers, and ignores key releases. The cooked path reuses scancodeAscii().
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
	bool m_shift;  // a Shift key (left 0x2A or right 0x36) is held
public:
	KeyDecoder() : m_ext(false), m_ctrl(false), m_shift(false) {}

	// US scancode (set 1) -> ASCII, or 0 for non-text keys. '\b' for Backspace,
	// '\n' for Enter. scancodeAsciiShift is the Shift-held variant (uppercase letters
	// + upper-glyph symbols). Both defined in KeyDecoder.cpp.
	static char scancodeAscii(unsigned char sc);
	static char scancodeAsciiShift(unsigned char sc);

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
		if (sc == 0x2A || sc == 0x36) { m_shift = true;  return; }   // Shift press (L/R)
		if (sc == 0xAA || sc == 0xB6) { m_shift = false; return; }   // Shift release (L/R)
		if (sc & 0x80)
			return;                  // key release
		// Ctrl combos key off the UNSHIFTED letter so Ctrl+C works whether or not Shift is
		// held (and 'A'&0x1F == 'a'&0x1F anyway); plain text uses the shifted glyph.
		char base = scancodeAscii(sc);
		if (m_ctrl && ((base >= 'a' && base <= 'z') || base == '\\')) {
			emit(base & 0x1F);   // Ctrl+letter / Ctrl+\ -> control code (Ctrl+C=0x03, Ctrl+\=0x1C)
			return;
		}
		char a = m_shift ? scancodeAsciiShift(sc) : base;
		if (a == 0)
			return;
		emit((int) (unsigned char) (a == '\b' ? 0x08 : a));
	}
};

}  // namespace kernel

#endif /* KEYDECODER_H_ */
