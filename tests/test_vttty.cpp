#include "doctest.h"
#include "VtTty.h"
#include "vt/VtManager.h"
#include "vt/VtIoctl.h"
#include "Termios.h"
#include "Process.h"
#include "Pty.h"
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
	CHECK(tty.ioctl(VT_ACTIVATE, (void*) 4) == 0);   // VT_ACTIVATE: VT number BY VALUE (Linux ABI)
	CHECK(m->active() == 4);
	vt_stat st;
	CHECK(tty.ioctl(VT_GETSTATE, &st) == 0);
	CHECK(st.v_active == 4);
}

TEST_CASE("KDSETMODE/KDGETMODE round-trips the console graphics mode") {
	static unsigned char buf[80*64*4];
	VtManager* m = mgr(buf);
	VtTty tty(3);
	CHECK(tty.ioctl(KDSETMODE, (void*) (long) KD_GRAPHICS) == 0);   // KDSETMODE: mode BY VALUE
	int got = 0;
	CHECK(tty.ioctl(KDGETMODE, &got) == 0);                        // KDGETMODE: BY POINTER
	CHECK(got == KD_GRAPHICS);
	CHECK(m->vt(3)->mode() == KD_GRAPHICS);
}

TEST_CASE("tty0 resolves to the active VT") {
	static unsigned char buf[80*64*4];
	VtManager* m = mgr(buf);
	VtTty(1).ioctl(VT_ACTIVATE, (void*) 5);          // by value
	VtTty active(0);                            // /dev/tty0
	int pg = 77;
	CHECK(active.ioctl(IOCTL_TIOCSPGRP, &pg) == 0);
	CHECK(m->vt(5)->fgPgrp() == 77);           // routed to VT5 (the active one)
}

TEST_CASE("/dev/tty (ControllingTty) is ENXIO until TIOCSCTTY, then forwards to the adopted VT") {
	static unsigned char buf[80*64*4];
	VtManager* m = mgr(buf);
	ProcTable::init();
	Process* p = ProcTable::alloc(0);
	ProcTable::setCurrent(p);

	ControllingTty ctty;                       // /dev/tty
	// No controlling terminal yet -> every op reports ENXIO / not-ready.
	CHECK(p->cttyDev == nullptr);
	int pg = 0;
	CHECK(ctty.ioctl(IOCTL_TIOCGPGRP, &pg) == -ENXIO);
	char c;
	CHECK(ctty.read(0, &c, 1) == -ENXIO);
	CHECK(ctty.pollReady(POLLIN) == 0);

	// A getty adopts /dev/tty2 as its controlling terminal.
	VtTty tty2(2);
	CHECK(tty2.ioctl(IOCTL_TIOCSCTTY, (void*) 0) == 0);
	CHECK(p->cttyDev == &tty2);

	// /dev/tty now forwards to VT2: setting its fg pgrp lands on VT2.
	pg = 91;
	CHECK(ctty.ioctl(IOCTL_TIOCSPGRP, &pg) == 0);
	CHECK(m->vt(2)->fgPgrp() == 91);
}

TEST_CASE("/dev/tty forwards to a pty slave when the controlling terminal is the pty") {
	static unsigned char buf[80*64*4];
	mgr(buf);
	ProcTable::init();
	Process* p = ProcTable::alloc(0);
	ProcTable::setCurrent(p);

	Pty pty;
	PtySlave slave(&pty);
	// The shell (login_tty/nwterm) adopts the pty slave as its controlling terminal.
	CHECK(slave.ioctl(IOCTL_TIOCSCTTY, (void*) 0) == 0);
	CHECK(p->cttyDev == &slave);

	// /dev/tty now reads/writes the pty, NOT a VT: a line written to the master surfaces on
	// the slave (cooked line discipline flushes on '\n'), and a read of /dev/tty returns it.
	ControllingTty ctty;
	const char* in = "x\n";
	CHECK(pty.masterWrite(in, 2) == 2);        // emulator keystrokes -> slave input
	char got[8] = {0};
	int r = ctty.read(0, got, sizeof got);     // /dev/tty -> slave read
	CHECK(r == 2);
	CHECK(got[0] == 'x');
	CHECK(got[1] == '\n');
}
