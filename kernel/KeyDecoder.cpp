#include "KeyDecoder.h"

namespace kernel {

// US QWERTY, PS/2 scancode set 1. Non-text keys (modifiers, function keys, Esc,
// Tab) map to 0; Backspace -> '\b', Enter -> '\n'. The shift state is tracked by
// feed() (KeyDecoder.h), which picks the shifted table below when a Shift is held.
char KeyDecoder::scancodeAscii(unsigned char sc) {
	static const char t[128] = {
		0,    0x1b, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',  // sc 1 = ESC
		'q',  'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,  'a',  's',
		'd',  'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', 0,  0,   '\\','z', 'x', 'c',  'v',
		'b',  'n', 'm', ',', '.', '/', 0,   0,   0,   ' ', 0,   0,   0,   0,   0,    0,
	};
	return sc < 128 ? t[sc] : 0;
}

// Shifted US QWERTY: letters -> uppercase, the number row and punctuation -> their
// upper-glyph symbols. Same layout/indices as scancodeAscii. Essential for a shell —
// '$', '|', '>', '*', '(', etc. and uppercase all require Shift.
char KeyDecoder::scancodeAsciiShift(unsigned char sc) {
	static const char t[128] = {
		0,    0x1b, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',  // sc 1 = ESC
		'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,  'A',  'S',
		'D',  'F', 'G', 'H', 'J', 'K', 'L', ':', '"', 0,   0,   '|', 'Z', 'X', 'C',  'V',
		'B',  'N', 'M', '<', '>', '?', 0,   0,   0,   ' ', 0,   0,   0,   0,   0,    0,
	};
	return sc < 128 ? t[sc] : 0;
}

}  // namespace kernel
