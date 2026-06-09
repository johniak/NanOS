/*
 * racetest — terminal stress / scheduler self-test. Does htop-style repeated full-screen
 * clear + full colour redraw for many rounds inside nterm, while keeping a register loop
 * counter alongside a volatile in-memory shadow. It verifies that sustained heavy concurrent
 * rendering (emulator CPU-bound on the framebuffer while this program floods the pty) does
 * not corrupt the program's state under preemption: a screendump during the final hold shows
 * "rounds=N/N mism=0", i.e. every round ran and the counter never diverged from its shadow.
 */
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <stdio.h>

static void out(const char* s, int n) {
	int off = 0;
	while (off < n) {
		int w = write(1, s + off, n - off);
		if (w > 0) off += w;
		else { struct timespec d = { 0, 1000000 }; nanosleep(&d, 0); }
	}
}
static void emit(const char* fmt, ...) {
	char buf[256];
	va_list ap; va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n > (int) sizeof buf) n = sizeof buf;
	out(buf, n);
}

#define ROUNDS 60
static volatile unsigned shadow, rounds, mism;

int main(void) {
	out("\x1b[?1049h", 8);
	rounds = 0; mism = 0;
	unsigned r;
	for (r = 0; r < ROUNDS; r++) {
		shadow = r;
		out("\x1b[2J", 4);                       // full clear (heavy clearRegion in nterm)
		for (int row = 1; row <= 40; row++)      // redraw a full screen of coloured cells
			for (int col = 1; col <= 100; col += 4)
				emit("\x1b[%d;%dH\x1b[48;5;%dm  \x1b[0m", row, col, 16 + ((r + row + col) % 200));
		if (r != shadow) mism++;
		rounds = rounds + 1;
	}
	emit("\x1b[24;1H\x1b[41;97m racetest STRESS: rounds=%u/%u mism=%u \x1b[0m", rounds, (unsigned) ROUNDS, mism);
	struct timespec ts = { 6, 0 };
	nanosleep(&ts, 0);
	out("\x1b[?1049l", 8);
	return 0;
}
