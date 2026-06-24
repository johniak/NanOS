#include "doctest.h"
#include "vt/VtConsole.h"
#include "vt/VtIoctl.h"
#include "Framebuffer.h"
#include "Syscall.h"   // EAGAIN
using namespace kernel;

static FbSurface surf(unsigned char* b, int w, int h) {
	return FbSurface{ b, (unsigned)(w*4), (unsigned)w, (unsigned)h, 32 };
}

TEST_CASE("VtConsole starts as a text console with no fg pgrp and is created off-screen") {
	static unsigned char buf[80*64*4];
	VtConsole vt;
	vt.init(surf(buf, 80, 64), /*index=*/2);
	CHECK(vt.index() == 2);
	CHECK(vt.mode() == KD_TEXT);
	CHECK(vt.fgPgrp() == 0);
	CHECK_FALSE(vt.fbcon().live());        // VtManager makes exactly one live
}

TEST_CASE("VtConsole fg pgrp and KD mode are independently settable") {
	static unsigned char buf[80*64*4];
	VtConsole vt;
	vt.init(surf(buf, 80, 64), 7);
	vt.setFgPgrp(42);   CHECK(vt.fgPgrp() == 42);
	vt.setMode(KD_GRAPHICS); CHECK(vt.mode() == KD_GRAPHICS);
}

TEST_CASE("raw-mode VtConsole buffers decoded bytes and read() drains them") {
	static unsigned char buf[80*64*4];
	VtConsole vt; vt.init(surf(buf, 80, 64), 1);
	vt.setRaw(true);
	// scancodes for 'h','i' (set-1 make codes: h=0x23, i=0x17)
	vt.feedScancode(0x23); vt.feedScancode(0x17);
	CHECK(vt.inputReady());
	char out[8]; int r = vt.read(out, sizeof out, /*nonblock=*/true);
	CHECK(r == 2);
	CHECK(out[0] == 'h'); CHECK(out[1] == 'i');
}

TEST_CASE("nonblocking read with no data returns -EAGAIN in raw mode") {
	static unsigned char buf[80*64*4];
	VtConsole vt; vt.init(surf(buf, 80, 64), 1);
	vt.setRaw(true);
	char out[8];
	CHECK(vt.read(out, sizeof out, true) == -EAGAIN);
}
