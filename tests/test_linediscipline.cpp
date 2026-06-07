#include "doctest.h"
#include "LineDiscipline.h"
#include <cstring>

using kernel::LineDiscipline;

// A tiny echo recorder used as the injected sink.
namespace {
struct Echo {
	char buf[128];
	int n = 0;
	void operator()(char c) { if (n < (int) sizeof(buf)) buf[n++] = c; }
	bool is(const char* s) const {
		int len = (int) strlen(s);
		return len == n && memcmp(buf, s, len) == 0;
	}
};
}

TEST_CASE("printable chars accumulate; commit on newline") {
	LineDiscipline ld;
	Echo echo;
	ld.push('a', echo);
	ld.push('b', echo);
	CHECK(!ld.lineReady());
	ld.push('\n', echo);
	CHECK(ld.lineReady());

	ld.push('z', echo);                  // ignored until takeLine consumes the line
	CHECK(echo.is("ab\n"));

	char out[16];
	int n = ld.takeLine(out, sizeof out);
	CHECK(n == 3);                       // "ab\n"
	CHECK(memcmp(out, "ab\n", 3) == 0);
	CHECK(!ld.lineReady());              // consumed
	CHECK(echo.is("ab\n"));
}

TEST_CASE("backspace erases last char and echoes \\b \\b; no-op when empty") {
	LineDiscipline ld;
	Echo echo;
	ld.push('\b', echo);                 // empty: ignored
	CHECK(echo.n == 0);
	ld.push('x', echo);
	ld.push('\b', echo);
	ld.push('\n', echo);

	char out[16];
	int n = ld.takeLine(out, sizeof out);
	CHECK(n == 1);                       // just "\n"
	CHECK(out[0] == '\n');
	// echo: 'x', then BS SP BS, then '\n'
	CHECK(echo.is("x\b \b\n"));
}

TEST_CASE("overflow drops chars past capacity but still commits") {
	LineDiscipline ld;
	Echo echo;
	for (int i = 0; i < LineDiscipline::CAP + 50; i++)
		ld.push('a', echo);
	ld.push('\n', echo);

	char out[LineDiscipline::CAP + 8];
	int n = ld.takeLine(out, sizeof out);
	CHECK(n == LineDiscipline::CAP);     // capacity incl. the newline slot
}

TEST_CASE("ctrl-D on empty line signals EOF (zero-length line ready)") {
	LineDiscipline ld;
	Echo echo;
	ld.push(4 /*EOT*/, echo);
	CHECK(ld.lineReady());

	char out[8];
	CHECK(ld.takeLine(out, sizeof out) == 0);
}
