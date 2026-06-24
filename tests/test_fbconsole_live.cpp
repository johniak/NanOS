#include "doctest.h"
#include "FbConsole.h"
#include "Framebuffer.h"
using namespace kernel;

// A heap-backed FbSurface so blits write somewhere we can inspect.
static FbSurface makeSurface(unsigned char* buf, int w, int h) {
	return FbSurface{ buf, (unsigned)(w * 4), (unsigned)w, (unsigned)h, 32 };
}

TEST_CASE("inactive FbConsole updates its grid but does not blit; repaintAll flushes it") {
	const int W = 8 * 10, H = 16 * 4;            // 10 cols x 4 rows
	static unsigned char buf[W * H * 4];
	for (unsigned i = 0; i < sizeof buf; i++) buf[i] = 0;

	FbConsole c;
	c.init(makeSurface(buf, W, H));
	c.setLive(false);                            // off-screen
	// clear the surface to a known sentinel so we can detect "no blit happened"
	for (unsigned i = 0; i < sizeof buf; i++) buf[i] = 0xAB;

	c.putChar('X');                              // updates grid, must NOT touch the surface
	bool touched = false;
	for (unsigned i = 0; i < sizeof buf; i++) if (buf[i] != 0xAB) { touched = true; break; }
	CHECK_FALSE(touched);                        // inactive: surface untouched
	CHECK(c.cursorX() == 1);                     // but the grid advanced

	c.repaintAll();                              // becoming visible flushes the whole grid
	bool nowTouched = false;
	for (unsigned i = 0; i < sizeof buf; i++) if (buf[i] != 0xAB) { nowTouched = true; break; }
	CHECK(nowTouched);                           // glyph 'X' is now on the surface
	CHECK(c.live());
}
