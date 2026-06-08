/*
 * sigtest — a tiny program to exercise catchable signals in QEMU.
 *
 *   sigtest          install a SIGINT handler, then spin until it fires (then exit 0).
 *   sigtest nocatch  leave SIGINT at its default action (Ctrl+C terminates the process).
 *
 * The handler uses write() (not printf) to stay async-signal-safe.
 */
#include <signal.h>
#include <string.h>
#include <unistd.h>

static volatile int g_got;

static void on_sigint(int sig) {
	(void) sig;
	static const char msg[] = "[sigtest] caught SIGINT\n";
	write(1, msg, sizeof msg - 1);
	g_got = 1;
}

int main(int argc, char** argv) {
	if (argc > 1 && !strcmp(argv[1], "nocatch")) {
		static const char m[] = "[sigtest] default SIGINT, spinning\n";
		write(1, m, sizeof m - 1);
		for (;;) { /* spin; Ctrl+C should terminate us */ }
	}

	signal(SIGINT, on_sigint);
	static const char m[] = "[sigtest] handler installed, spinning\n";
	write(1, m, sizeof m - 1);
	while (!g_got) { /* spin until the handler runs */ }

	static const char done[] = "[sigtest] handler ran, exiting 0\n";
	write(1, done, sizeof done - 1);
	return 0;
}
