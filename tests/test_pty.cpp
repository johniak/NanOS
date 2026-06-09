#include "doctest.h"
#include "Pty.h"
#include "Termios.h"
#include "Signal.h"
#include <cstring>

using namespace kernel;

static int g_sig;
static void capSig(void*, int sig, int) { g_sig = sig; }

TEST_CASE("Pty canonical mode commits a whole line to the slave + echoes to the master") {
	Pty p;
	p.masterWrite("ls\n", 3);
	char in[16] = {0};
	int n = p.slaveRead(in, sizeof in);
	REQUIRE(n == 3);
	CHECK(strncmp(in, "ls\n", 3) == 0);
	char ech[16] = {0};
	int e = p.masterRead(ech, sizeof ech);   // echo "ls" then CRLF
	REQUIRE(e == 4);
	CHECK(strncmp(ech, "ls\r\n", 4) == 0);
}

TEST_CASE("Pty canonical slaveRead blocks (EAGAIN) until newline") {
	Pty p;
	p.masterWrite("ab", 2);                  // no newline yet -> nothing committed
	char in[8];
	CHECK(p.slaveRead(in, 8) == -EAGAIN);
	p.masterWrite("\n", 1);
	CHECK(p.slaveRead(in, 8) == 3);          // now "ab\n"
}

TEST_CASE("Pty backspace edits the pending canonical line") {
	Pty p;
	p.masterWrite("ab\b\n", 4);              // a, b, erase b, commit -> "a\n"
	char in[8] = {0};
	int n = p.slaveRead(in, 8);
	REQUIRE(n == 2);
	CHECK(in[0] == 'a');
	CHECK(in[1] == '\n');
}

TEST_CASE("Pty raw mode passes bytes through immediately with no echo") {
	Pty p;
	Termios t;
	p.ioctl(IOCTL_TCGETS, &t);
	t.c_lflag &= ~(TL_ICANON | TL_ECHO);
	p.ioctl(IOCTL_TCSETS, &t);
	p.masterWrite("x", 1);
	char in[4] = {0};
	CHECK(p.slaveRead(in, 4) == 1);
	CHECK(in[0] == 'x');
	char ech[4];
	CHECK(p.masterRead(ech, 4) == -EAGAIN);  // no echo
}

TEST_CASE("Pty Ctrl+C raises SIGINT to the foreground group") {
	Pty p;
	g_sig = 0;
	p.setSignalFn(capSig, 0);
	p.masterWrite("\x03", 1);                // VINTR
	CHECK(g_sig == SIGINT);
}

TEST_CASE("Pty maps NL->CRLF on slave output (ONLCR)") {
	Pty p;
	p.slaveWrite("hi\n", 3);
	char out[8] = {0};
	int n = p.masterRead(out, sizeof out);
	REQUIRE(n == 4);
	CHECK(strncmp(out, "hi\r\n", 4) == 0);
}

TEST_CASE("Pty winsize ioctl round-trips") {
	Pty p;
	Winsize w = { 40, 100, 0, 0 };
	CHECK(p.ioctl(IOCTL_TIOCSWINSZ, &w) == 0);
	Winsize r = { 0, 0, 0, 0 };
	CHECK(p.ioctl(IOCTL_TIOCGWINSZ, &r) == 0);
	CHECK(r.ws_row == 40);
	CHECK(r.ws_col == 100);
}
