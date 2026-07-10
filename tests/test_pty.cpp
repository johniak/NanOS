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

TEST_CASE("Pty raw masterWrite is a partial write when the input ring fills, never dropping") {
	Pty p;
	Termios t;
	p.ioctl(IOCTL_TCGETS, &t);
	t.c_lflag &= ~(TL_ICANON | TL_ECHO);         // raw: bytes go straight to the slave ring
	p.ioctl(IOCTL_TCSETS, &t);
	char big[5000];
	for (int i = 0; i < 5000; i++) big[i] = 'y';
	// The 4096-byte input ring fills: masterWrite returns the count that fit (partial), then
	// -EAGAIN once full, so a bulk writer (paste / pipe into the tty) loses no bytes.
	int w = p.masterWrite(big, 5000);
	CHECK(w == 4096);
	CHECK(p.masterWrite(big, 1) == -EAGAIN);     // full -> would block, not a silent drop
	char in[4096];
	CHECK(p.slaveRead(in, sizeof in) == 4096);   // drain
	CHECK(p.masterWrite(big, 10) == 10);         // room again
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

TEST_CASE("Pty slaveWrite is a partial write when the ring is nearly full, never dropping bytes") {
	Pty p;
	char big[5000];
	for (int i = 0; i < 5000; i++) big[i] = 'x';
	// First chunk fits the 4096-byte ring; a second chunk that would overflow writes only
	// what fits (partial) so the caller's stdio loops on the remainder -- no bytes lost.
	int w1 = p.slaveWrite(big, 3000);
	CHECK(w1 == 3000);
	int w2 = p.slaveWrite(big, 2000);            // only 1096 slots left
	CHECK(w2 == 1096);
	// Ring now completely full: a further write would block (-EAGAIN) until drained.
	CHECK(p.slaveWrite(big, 1) == -EAGAIN);
	// Drain the master, then the rest fits.
	char out[4096];
	int r = p.masterRead(out, sizeof out);
	CHECK(r == 4096);
	CHECK(p.slaveWrite(big, 904) == 904);        // remainder of the 2000-byte write
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

TEST_CASE("Pty TIOCPKT packet mode prefixes a status byte on master reads") {
	Pty p;
	int on = 1;
	CHECK(p.ioctl(IOCTL_TIOCPKT, &on) == 0);
	// Slave writes "hi"; in packet mode the master read is [TIOCPKT_DATA][h][i].
	CHECK(p.slaveWrite("hi", 2) == 2);
	unsigned char out[8] = {0};
	int r = p.masterRead(out, sizeof out);
	CHECK(r == 3);
	CHECK(out[0] == (unsigned char) TIOCPKT_DATA);   // 0 = ordinary data, no flush
	CHECK(out[1] == 'h');
	CHECK(out[2] == 'i');
	// Turning packet mode off restores the raw stream (no preamble).
	int off = 0;
	CHECK(p.ioctl(IOCTL_TIOCPKT, &off) == 0);
	CHECK(p.slaveWrite("ok", 2) == 2);
	r = p.masterRead(out, sizeof out);
	CHECK(r == 2);
	CHECK(out[0] == 'o');
	CHECK(out[1] == 'k');
}

TEST_CASE("PtyMaster/PtySlave CharDevice adapters forward to the shared Pty") {
	// Covers the thin inline adapters (drivers/Pty.h): read/write/poll/waitQueue/mmapInfo on
	// both device faces route to the one Pty ring pair.
	Pty p;
	PtyMaster m(&p);
	PtySlave s(&p);
	CHECK(m.write(0, "hi\n", 3) == 3);       // master -> slave (canonical commit on \n)
	char in[8] = {0};
	CHECK(s.read(0, in, sizeof in) == 3);
	CHECK(strncmp(in, "hi\n", 3) == 0);
	char ech[16] = {0};
	CHECK(m.read(0, ech, sizeof ech) > 0);   // the echo came back on the master face
	CHECK(s.write(0, "ok", 2) == 2);         // slave -> master
	char out[8] = {0};
	CHECK(m.read(0, out, sizeof out) == 2);
	CHECK(strncmp(out, "ok", 2) == 0);
	uint64_t ph; unsigned ln;
	CHECK(m.mmapInfo(&ph, &ln) == -1);       // ptys are not mappable
	CHECK(s.mmapInfo(&ph, &ln) == -1);
	CHECK(m.waitQueue() == p.waitQueue());   // both faces park on the one wait list
	CHECK(s.waitQueue() == p.waitQueue());
}
