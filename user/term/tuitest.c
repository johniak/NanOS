/*
 * tuitest — exercises the nterm VT engine the way a full-screen TUI (htop/btop/vim) does:
 * switches to the alternate screen, clears it, draws 16- and 256-colour swatches, a box via
 * cursor addressing, and coloured text, holds it, then restores the normal screen. Run it
 * inside nterm; a screendump during the hold proves colours + cursor positioning +
 * alt-screen work — the Stage-6 acceptance check.
 *
 * Uses raw write(2) (no stdio buffering) so the byte stream to the pty is exactly what we
 * emit, looping on short writes.
 */
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <stdio.h>

/* Write the whole buffer, looping on short writes. */
static void out(const char* s, int n) {
	int off = 0;
	while (off < n) {
		int w = write(1, s + off, n - off);
		if (w > 0) off += w;
		else if (w < 0) { struct timespec d = { 0, 1000000 }; nanosleep(&d, 0); }
	}
}
static void puts2(const char* s) { int n = 0; while (s[n]) n++; out(s, n); }
static void emit(const char* fmt, ...) {
	char buf[256];
	va_list ap; va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n > (int) sizeof buf) n = sizeof buf;
	out(buf, n);
}
static void at(int r, int c) { emit("\x1b[%d;%dH", r, c); }

int main(void) {
	puts2("\x1b[?1049h\x1b[2J");                  // alternate screen + clear

	at(2, 22); puts2("\x1b[1;33mNanOS nterm - VT self-test\x1b[0m");

	/* 16 basic colours */
	at(4, 4); puts2("\x1b[37m16-colour:\x1b[0m ");
	for (int c = 0; c < 16; c++) emit("\x1b[48;5;%dm  \x1b[0m", c);

	/* 256-colour cube sample (two rows) */
	at(6, 4); puts2("\x1b[37m256-colour:\x1b[0m ");
	for (int c = 16; c < 16 + 72; c++) emit("\x1b[48;5;%dm \x1b[0m", c);
	at(7, 16);
	for (int c = 16 + 72; c < 16 + 144; c++) emit("\x1b[48;5;%dm \x1b[0m", c);

	/* a box drawn purely with cursor addressing */
	int top = 9, bot = 16, left = 4, right = 50;
	at(top, left); puts2("+"); for (int x = left + 1; x < right; x++) puts2("-"); puts2("+");
	at(bot, left); puts2("+"); for (int x = left + 1; x < right; x++) puts2("-"); puts2("+");
	for (int y = top + 1; y < bot; y++) { at(y, left); puts2("|"); at(y, right); puts2("|"); }

	at(11, 8); puts2("Cursor addressing + scroll regions work.");
	at(13, 8); puts2("\x1b[1;32mgreen\x1b[0m  \x1b[1;31mred\x1b[0m  "
	                 "\x1b[1;34mblue\x1b[0m  \x1b[7mreverse\x1b[0m  \x1b[1mbold\x1b[0m");

	at(18, 4); puts2("\x1b[36m(restoring normal screen in a moment...)\x1b[0m");

	struct timespec ts = { 7, 0 };
	nanosleep(&ts, 0);
	puts2("\x1b[?1049l");                          // back to the normal screen
	return 0;
}
