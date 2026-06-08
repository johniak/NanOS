#include "doctest.h"
#include "Font8x16.h"

using namespace kernel;

TEST_CASE("fontGlyph: space is blank, 'A' matches the known VGA bitmap") {
	const unsigned char* sp = fontGlyph(' ');
	for (int i = 0; i < FONT_H; i++)
		CHECK(sp[i] == 0);                     // space glyph is all-zero

	const unsigned char* a = fontGlyph('A');   // 00 00 10 38 6c c6 c6 fe c6 c6 c6 c6 00..
	CHECK(a[3] == 0x38);
	CHECK(a[5] == 0xc6);
	CHECK(a[7] == 0xfe);
	CHECK(a[15] == 0x00);
	int set = 0;
	for (int i = 0; i < FONT_H; i++) if (a[i]) set++;
	CHECK(set > 0);
}

TEST_CASE("fontGlyph: bounds 0 and 255 are valid 16-byte lookups") {
	CHECK(fontGlyph(0) != nullptr);
	CHECK(fontGlyph(255) != nullptr);
	// distinct glyphs live 16 bytes apart
	CHECK(fontGlyph(2) - fontGlyph(1) == FONT_H);
}
