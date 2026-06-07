/*
 * arch/console.h — MI/MD contract for the kernel console sink.
 *
 * The arch provides a character cell sink (place a glyph, advance, scroll, move
 * the hardware cursor). Numeric/string formatting lives in the MI `Console`
 * class on top of these. x86 implements this over the VGA text buffer; another
 * arch would implement it over a UART or framebuffer.
 */
#pragma once

namespace arch {

void consoleInit();                          // clear + reset cursor
void consolePutChar(char c);                 // interpret \b \t \r \n, place glyph, scroll, move cursor
void consoleClear();
void consoleSetCursor(unsigned x, unsigned y);

}
