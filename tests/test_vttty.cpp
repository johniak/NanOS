#include "doctest.h"
#include "VtTty.h"
#include "vt/VtManager.h"
#include "vt/VtIoctl.h"
#include "Termios.h"
using namespace kernel;

static VtManager* mgr(unsigned char* buf) {
	static VtManager m;
	FbSurface s{ buf, 80*4, 80, 64, 32 };
	m.init(s, 0);
	g_vtmgr = &m;
	return &m;
}

TEST_CASE("VtTty TIOCSPGRP/TIOCGPGRP set and read its VT's foreground process group") {
	static unsigned char buf[80*64*4];
	VtManager* m = mgr(buf);
	VtTty tty(2);                              // /dev/tty2
	int pg = 55;
	CHECK(tty.ioctl(IOCTL_TIOCSPGRP, &pg) == 0);
	int got = 0;
	CHECK(tty.ioctl(IOCTL_TIOCGPGRP, &got) == 0);
	CHECK(got == 55);
	CHECK(m->vt(2)->fgPgrp() == 55);
}

TEST_CASE("VT_ACTIVATE via ioctl switches the active console; VT_GETSTATE reports it") {
	static unsigned char buf[80*64*4];
	VtManager* m = mgr(buf);
	VtTty tty(1);
	int n = 4;
	CHECK(tty.ioctl(VT_ACTIVATE, &n) == 0);
	CHECK(m->active() == 4);
	vt_stat st;
	CHECK(tty.ioctl(VT_GETSTATE, &st) == 0);
	CHECK(st.v_active == 4);
}

TEST_CASE("KDSETMODE/KDGETMODE round-trips the console graphics mode") {
	static unsigned char buf[80*64*4];
	VtManager* m = mgr(buf);
	VtTty tty(3);
	int mode = KD_GRAPHICS;
	CHECK(tty.ioctl(KDSETMODE, &mode) == 0);
	int got = 0;
	CHECK(tty.ioctl(KDGETMODE, &got) == 0);
	CHECK(got == KD_GRAPHICS);
	CHECK(m->vt(3)->mode() == KD_GRAPHICS);
}

TEST_CASE("tty0 resolves to the active VT") {
	static unsigned char buf[80*64*4];
	VtManager* m = mgr(buf);
	int n = 5; VtTty(1).ioctl(VT_ACTIVATE, &n);
	VtTty active(0);                            // /dev/tty0
	int pg = 77;
	CHECK(active.ioctl(IOCTL_TIOCSPGRP, &pg) == 0);
	CHECK(m->vt(5)->fgPgrp() == 77);           // routed to VT5 (the active one)
}
