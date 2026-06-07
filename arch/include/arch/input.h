/*
 * arch/input.h — MI/MD contract for blocking console input.
 *
 * The arch keyboard driver fills a cooked-mode line buffer (LineDiscipline). MI
 * code (the read(0) syscall) blocks here until a full line is available, then
 * copies it out. Blocking is arch-specific (x86: sti + hlt while the IRQ fills
 * the buffer).
 */
#pragma once

namespace arch {

// Read console input, blocking until something is available. Mode-dependent:
//  - cooked (default): returns one committed, line-edited line (echoed by the kernel);
//  - raw: returns >=1 available raw bytes (no echo, no line editing); arrow keys
//    arrive as ANSI escape sequences (ESC '[' 'A'/'B'/'C'/'D').
// Returns the number of bytes copied (0 = EOF on Ctrl-D at an empty cooked line).
int inputRead(char* buf, unsigned n);

// Select console input mode: 0 = cooked, 1 = raw. A shell switches to raw to run
// its own line editor, and back to cooked while a child program runs.
void inputSetRaw(int raw);

}
