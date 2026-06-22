#include "doctest.h"
#include "vt.h"
#include <cstring>

static void feed(vt& t, const char* s) { vt_feed(&t, (const unsigned char*) s, (int) strlen(s)); }

TEST_CASE("vt: printable text fills the grid and advances the cursor") {
	vt t; vt_init(&t, 80, 25);
	feed(t, "abc");
	CHECK(t.grid[0][0].ch == 'a');
	CHECK(t.grid[0][1].ch == 'b');
	CHECK(t.grid[0][2].ch == 'c');
	CHECK(t.cx == 3);
	CHECK(t.cy == 0);
}

TEST_CASE("vt: OSC, xterm private markers, and CSI intermediates are swallowed whole") {
	vt t; vt_init(&t, 80, 25);
	// OSC set-title (BEL-terminated) must not leak its payload as text.
	feed(t, "\x1b]0;my title\x07X");
	CHECK(t.grid[0][0].ch == 'X');
	CHECK(t.cx == 1);
	// A '>' private CSI (modifyOtherKeys) + a DECSCUSR with a space intermediate: fully consumed,
	// then a normal glyph lands right after — no stray digits/letters from the sequences.
	vt_init(&t, 80, 25);
	feed(t, "\x1b[>4;2m\x1b[6 qY");
	CHECK(t.grid[0][0].ch == 'Y');
	CHECK(t.cx == 1);
	// A 256-colour SGR still colours the next glyph (regression guard for the leak fix).
	vt_init(&t, 80, 25);
	feed(t, "\x1b[38;5;4mZ");
	CHECK(t.grid[0][0].ch == 'Z');
	CHECK(t.grid[0][0].fg == 4);
}

TEST_CASE("vt: charset-designation escapes (ESC ( B etc.) are swallowed, not leaked") {
	vt t; vt_init(&t, 80, 25);
	// ncurses emits `\033(B` (designate G0 = US-ASCII) as part of every attribute reset. The
	// trailing selector 'B' is a *final* byte and must be consumed, not printed as a glyph.
	feed(t, "\x1b(BX");
	CHECK(t.grid[0][0].ch == 'X');
	CHECK(t.cx == 1);
	// G1 designation `ESC ) 0` (VT100 line-drawing) likewise swallows its '0' selector.
	vt_init(&t, 80, 25);
	feed(t, "\x1b)0Y");
	CHECK(t.grid[0][0].ch == 'Y');
	CHECK(t.cx == 1);
	// ESC '#' '8' (DECALN) — ESC + intermediate '#' + final '8' — is consumed whole too.
	vt_init(&t, 80, 25);
	feed(t, "\x1b#8Z");
	CHECK(t.grid[0][0].ch == 'Z');
	CHECK(t.cx == 1);
	// A realistic ncurses-style burst: SGR colour, charset reset, then text. Only the text lands,
	// in the chosen colour — no stray 'B' between the reset and the glyph (the htop bug).
	vt_init(&t, 80, 25);
	feed(t, "\x1b[32m\x1b(BCPU");
	CHECK(t.grid[0][0].ch == 'C');
	CHECK(t.grid[0][0].fg == 2);
	CHECK(t.grid[0][1].ch == 'P');
	CHECK(t.grid[0][2].ch == 'U');
	CHECK(t.cx == 3);
}

TEST_CASE("vt: CR/LF move the cursor; absolute CSI H positions it (1-based)") {
	vt t; vt_init(&t, 80, 25);
	feed(t, "hi\r\n");
	CHECK(t.cx == 0);
	CHECK(t.cy == 1);
	feed(t, "\x1b[5;3H");          // row 5, col 3 -> (cy=4, cx=2)
	CHECK(t.cy == 4);
	CHECK(t.cx == 2);
}

TEST_CASE("vt: SGR sets colours; ED clears the screen to spaces") {
	vt t; vt_init(&t, 80, 25);
	feed(t, "\x1b[31mR");          // red foreground
	CHECK(t.grid[0][0].ch == 'R');
	CHECK(t.grid[0][0].fg == 1);   // SGR 31 -> palette 1
	feed(t, "\x1b[0m");            // reset
	feed(t, "x\x1b[2J");           // write then erase-display
	CHECK(t.grid[0][0].ch == ' ');
}

TEST_CASE("vt: writing past the bottom scrolls the screen up") {
	vt t; vt_init(&t, 10, 3);
	feed(t, "top\r\n");            // row 0 = "top"
	feed(t, "\r\n\r\n\r\n");       // push past the last row -> scroll
	CHECK(t.cy == 2);              // clamped at the bottom row
	CHECK(t.grid[0][0].ch != 't' );  // "top" scrolled off the first row
}

TEST_CASE("vt: SGR 38;5;N selects a 256-colour index; truecolor downgrades") {
	vt t; vt_init(&t, 80, 25);
	feed(t, "\x1b[38;5;160mZ");
	CHECK(t.grid[0][0].fg == 160);
	feed(t, "\x1b[0m\x1b[38;2;255;0;0mQ");   // 24-bit red -> nearest cube index
	CHECK(t.grid[0][1].fg >= 16);            // mapped into the 256 cube
}

TEST_CASE("vt: resize changes geometry and clamps the cursor") {
	vt t; vt_init(&t, 80, 25);
	feed(t, "\x1b[20;70H");        // cursor near the far corner
	vt_resize(&t, 40, 10);
	CHECK(t.cols == 40);
	CHECK(t.rows == 10);
	CHECK(t.cx < 40);
	CHECK(t.cy < 10);
}

TEST_CASE("vt: relative cursor moves A/B/C/D clamp at the edges") {
	vt t; vt_init(&t, 20, 10);
	feed(t, "\x1b[5;5H");          // (cy=4, cx=4)
	feed(t, "\x1b[2A"); CHECK(t.cy == 2);
	feed(t, "\x1b[3B"); CHECK(t.cy == 5);
	feed(t, "\x1b[2C"); CHECK(t.cx == 6);
	feed(t, "\x1b[4D"); CHECK(t.cx == 2);
	feed(t, "\x1b[99A"); CHECK(t.cy == 0);   // clamp to top
	feed(t, "\x1b[99D"); CHECK(t.cx == 0);   // clamp to left
}

TEST_CASE("vt: EL (K) erases within the current line; save/restore cursor") {
	vt t; vt_init(&t, 20, 5);
	feed(t, "hello");
	feed(t, "\x1b[3G");            // column 3 (cx=2)
	feed(t, "\x1b[K");            // erase from cursor to EOL
	CHECK(t.grid[0][0].ch == 'h');
	CHECK(t.grid[0][2].ch == ' ');
	feed(t, "\x1b[2;2H\x1b[s");    // save cursor at (1,1)
	feed(t, "\x1b[5;5H");          // move away
	feed(t, "\x1b[u");            // restore
	CHECK(t.cy == 1); CHECK(t.cx == 1);
}

TEST_CASE("vt: BS and TAB move the cursor; reverse video swaps fg/bg") {
	vt t; vt_init(&t, 40, 5);
	feed(t, "ab\b");  CHECK(t.cx == 1);          // backspace
	feed(t, "\t");    CHECK(t.cx == 8);          // tab to next 8-stop
	feed(t, "\x1b[7mX");                          // reverse video
	CHECK(t.grid[0][8].fg == 0);                 // fg/bg swapped (default bg 0 -> fg)
}

TEST_CASE("vt: alternate screen (DECSET 1049) saves and restores the main screen") {
	vt t; vt_init(&t, 20, 5);
	feed(t, "shell$ ");                  // main screen content + cursor where it left off
	int mcx = t.cx, mcy = t.cy;
	CHECK(t.grid[0][0].ch == 's');
	feed(t, "\x1b[?1049h");              // a full-screen app starts: enter the alternate screen
	CHECK(t.alt == 1);
	CHECK(t.grid[0][0].ch == ' ');       // alt screen is blank
	feed(t, "\x1b[1;1HVIM");             // app draws into the alt screen
	CHECK(t.grid[0][0].ch == 'V');
	feed(t, "\x1b[?1049l");              // app exits: leave the alternate screen
	CHECK(t.alt == 0);
	CHECK(t.grid[0][0].ch == 's');       // main screen restored ('shell$ ')
	CHECK(t.grid[0][5].ch == '$');
	CHECK(t.cx == mcx);                  // and the main-screen cursor restored
	CHECK(t.cy == mcy);
}

TEST_CASE("vt: legacy alt-screen codes 47 and 1047 also save/restore") {
	vt t; vt_init(&t, 20, 5);
	feed(t, "main");
	feed(t, "\x1b[?47h");      CHECK(t.alt == 1); CHECK(t.grid[0][0].ch == ' ');
	feed(t, "\x1b[1;1HX");     CHECK(t.grid[0][0].ch == 'X');   // app homes the cursor itself
	feed(t, "\x1b[?47l");      CHECK(t.alt == 0); CHECK(t.grid[0][0].ch == 'm');
}
