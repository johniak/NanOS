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

// Block until a committed input line is available, then copy up to `n` bytes into
// `buf`. Returns the number of bytes copied (0 = EOF on Ctrl-D at an empty line).
int inputReadLine(char* buf, unsigned n);

}
