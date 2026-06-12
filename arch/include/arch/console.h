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

// Take the console over to the bootloader's linear framebuffer (Linux fbcon style).
// Must be called after the framebuffer MMIO is mapped; a no-op when there is none.
void consoleActivateFramebuffer();

// The console's character-cell dimensions, so a tty can answer TIOCGWINSZ (full-screen TUIs
// like vim size themselves off this). Reflects the framebuffer grid when active, else VGA text.
void consoleSize(unsigned* cols, unsigned* rows);

}
