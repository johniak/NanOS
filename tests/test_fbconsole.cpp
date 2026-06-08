#include "doctest.h"
#include "FbConsole.h"
#include "Font8x16.h"
#include <cstring>

using namespace kernel;

namespace {
const uint32_t W = FONT_W * 4, H = FONT_H * 3, PITCH = W * 4;   // 4 cols x 3 rows
}

TEST_CASE("FbConsole: grid size, glyph advance, newline") {
	unsigned char buf[PITCH * H];
	memset(buf, 0, sizeof buf);
	FbConsole con;
	con.init(FbSurface{ buf, PITCH, W, H, 32 });

	CHECK(con.cols() == 4);
	CHECK(con.rows() == 3);
	CHECK(con.cursorX() == 0);
	CHECK(con.cursorY() == 0);

	con.putChar('A');
	CHECK(con.cursorX() == 1);
	CHECK(con.cursorY() == 0);
	con.putChar('\n');
	CHECK(con.cursorX() == 0);
	CHECK(con.cursorY() == 1);
}

TEST_CASE("FbConsole: writing past the last column wraps to the next row") {
	unsigned char buf[PITCH * H];
	memset(buf, 0, sizeof buf);
	FbConsole con;
	con.init(FbSurface{ buf, PITCH, W, H, 32 });

	con.putChar('a'); con.putChar('b'); con.putChar('c'); con.putChar('d');
	CHECK(con.cursorX() == 0);   // 4 cols filled -> wrapped
	CHECK(con.cursorY() == 1);
}

TEST_CASE("FbConsole: scrolling clamps the cursor at the last row") {
	unsigned char buf[PITCH * H];
	memset(buf, 0, sizeof buf);
	FbConsole con;
	con.init(FbSurface{ buf, PITCH, W, H, 32 });

	con.putChar('\n');   // cy 1
	con.putChar('\n');   // cy 2
	con.putChar('\n');   // cy 3 -> scroll -> 2
	CHECK(con.cursorY() == 2);
	con.putChar('\n');   // still clamped after another scroll
	CHECK(con.cursorY() == 2);
}

TEST_CASE("FbConsole: backspace never underflows; setCursor clamps to the grid") {
	unsigned char buf[PITCH * H];
	memset(buf, 0, sizeof buf);
	FbConsole con;
	con.init(FbSurface{ buf, PITCH, W, H, 32 });

	con.putChar('A');           // cx 1
	con.putChar(0x08);          // backspace -> cx 0
	CHECK(con.cursorX() == 0);
	con.putChar(0x08);          // no underflow
	CHECK(con.cursorX() == 0);

	con.setCursor(99, 99);
	CHECK(con.cursorX() == con.cols() - 1);
	CHECK(con.cursorY() == con.rows() - 1);
	con.setCursor(2, 1);
	CHECK(con.cursorX() == 2);
	CHECK(con.cursorY() == 1);
}

TEST_CASE("FbConsole: a printed glyph paints pixels into its cell") {
	unsigned char buf[PITCH * H];
	memset(buf, 0, sizeof buf);
	FbConsole con;
	con.init(FbSurface{ buf, PITCH, W, H, 32 });   // bg = black

	con.putChar('A');                              // 'A' has set bits -> fg pixels
	bool any = false;
	for (uint32_t y = 0; y < FONT_H; y++)
		for (uint32_t x = 0; x < FONT_W; x++)
			if (*(uint32_t*) (buf + y * PITCH + x * 4) != 0)
				any = true;
	CHECK(any);
}
