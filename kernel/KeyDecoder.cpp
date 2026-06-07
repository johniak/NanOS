#include "KeyDecoder.h"

namespace kernel {

// US QWERTY, PS/2 scancode set 1. Non-text keys (modifiers, function keys, Esc,
// Tab) map to 0; Backspace -> '\b', Enter -> '\n'. Shifted symbols are not handled
// (no modifier state yet).
char KeyDecoder::scancodeAscii(unsigned char sc) {
	static const char t[128] = {
		0,    0,   '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', 0,
		'q',  'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,  'a',  's',
		'd',  'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', 0,  0,   '\\','z', 'x', 'c',  'v',
		'b',  'n', 'm', ',', '.', '/', 0,   0,   0,   ' ', 0,   0,   0,   0,   0,    0,
	};
	return sc < 128 ? t[sc] : 0;
}

}  // namespace kernel
