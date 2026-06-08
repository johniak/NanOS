/*
 * sigtest — a tiny program to exercise catchable signals in QEMU.
 *
 *   sigtest          install a SIGINT handler, then spin until it fires (then exit 0).
 *   sigtest nocatch  leave SIGINT at its default action (Ctrl+C terminates the process).
 *   sigtest read     install a handler, then block in read(); Ctrl+C runs the handler and
 *                    the read RESTARTS (SA_RESTART), so the typed line is still read.
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

	if (argc > 1 && !strcmp(argv[1], "read")) {
		signal(SIGINT, on_sigint);
		static const char m[] = "[sigtest] read mode: Ctrl+C, then type a line\n";
		write(1, m, sizeof m - 1);
		char buf[64];
		int n = read(0, buf, sizeof buf);    // blocks; SA_RESTART resumes it after the handler
		if (n < 0) {
			static const char e[] = "[sigtest] read failed (EINTR)\n";
			write(1, e, sizeof e - 1);
		} else {
			static const char g[] = "[sigtest] read got: ";
			write(1, g, sizeof g - 1);
			write(1, buf, n);
		}
		return 0;
	}

	signal(SIGINT, on_sigint);
	static const char m[] = "[sigtest] handler installed, spinning\n";
	write(1, m, sizeof m - 1);
	while (!g_got) { /* spin until the handler runs */ }

	static const char done[] = "[sigtest] handler ran, exiting 0\n";
	write(1, done, sizeof done - 1);
	return 0;
}
