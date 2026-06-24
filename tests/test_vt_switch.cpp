#include "doctest.h"
#include "vt/VtManager.h"
#include "vt/VtIoctl.h"
#include "Framebuffer.h"
using namespace kernel;

static int g_sigPid, g_sigNum, g_sigCount;
static void fakeSignal(int pid, int sig) { g_sigPid = pid; g_sigNum = sig; g_sigCount++; }

static VtManager* makeMgr(unsigned char* buf) {
	static VtManager m;
	FbSurface s{ buf, 80*4, 80, 64, 32 };
	m.init(s, fakeSignal);
	g_sigPid = g_sigNum = g_sigCount = 0;
	return &m;
}

TEST_CASE("active is VT1 after init; exactly VT1 is live") {
	static unsigned char buf[80*64*4];
	VtManager* m = makeMgr(buf);
	CHECK(m->active() == 1);
	CHECK(m->vt(1)->fbcon().live());
	CHECK_FALSE(m->vt(2)->fbcon().live());
}

TEST_CASE("text->text switch flips live bits and signals nothing") {
	static unsigned char buf[80*64*4];
	VtManager* m = makeMgr(buf);
	m->switchTo(3);
	CHECK(m->active() == 3);
	CHECK(m->vt(3)->fbcon().live());
	CHECK_FALSE(m->vt(1)->fbcon().live());
	CHECK(g_sigCount == 0);                 // no signals between text VTs
}

TEST_CASE("switch to a VT_PROCESS graphics VT sends its acquire signal") {
	static unsigned char buf[80*64*4];
	VtManager* m = makeMgr(buf);
	m->vt(7)->setMode(KD_GRAPHICS);
	m->vt(7)->setVtMode(VT_PROCESS, /*rel*/10, /*acq*/12, /*owner pid*/99);
	m->switchTo(7);
	CHECK(m->active() == 7);
	CHECK(g_sigCount == 1);
	CHECK(g_sigPid == 99);
	CHECK(g_sigNum == 12);                   // acqsig
}

TEST_CASE("switch away from a VT_PROCESS graphics VT requests release (relsig) and parks") {
	static unsigned char buf[80*64*4];
	VtManager* m = makeMgr(buf);
	m->vt(7)->setMode(KD_GRAPHICS);
	m->vt(7)->setVtMode(VT_PROCESS, 10, 12, 99);
	m->switchTo(7);                          // now on graphics
	g_sigCount = 0;
	bool completed = m->switchTo(2);         // request switch away
	CHECK_FALSE(completed);                  // not done until VT_RELDISP
	CHECK(g_sigNum == 10);                    // relsig sent
	CHECK(m->vt(7)->relWait());
	CHECK(m->active() == 7);                  // still graphics until the ack
	m->relDisp(7, /*release granted*/1);     // owner acks -> the pending switch completes
	CHECK(m->active() == 2);
	CHECK(m->vt(2)->fbcon().live());
	CHECK_FALSE(m->vt(7)->relWait());
}

TEST_CASE("a graphics owner that never acks VT_RELDISP is forced off after the deadline") {
	static unsigned char buf[80*64*4];
	VtManager* m = makeMgr(buf);
	m->vt(7)->setMode(KD_GRAPHICS);
	m->vt(7)->setVtMode(VT_PROCESS, 10, 12, 99);
	m->switchTo(7);                          // on graphics
	CHECK_FALSE(m->switchTo(2));             // pending release, owner never acks
	CHECK(m->active() == 7);
	for (int i = 0; i < kVtReleaseTimeoutTicks; i++) m->releaseTimeoutTick();
	CHECK(m->active() == 2);                 // forced over after the deadline elapsed
	CHECK(m->vt(2)->fbcon().live());
	CHECK_FALSE(m->vt(7)->relWait());
}

TEST_CASE("releaseTimeoutTick is a no-op when no switch is pending") {
	static unsigned char buf[80*64*4];
	VtManager* m = makeMgr(buf);
	m->switchTo(3);
	for (int i = 0; i < 2 * kVtReleaseTimeoutTicks; i++) m->releaseTimeoutTick();
	CHECK(m->active() == 3);                 // nothing pending -> stays put
}

TEST_CASE("switching to the already-active VT is a no-op") {
	static unsigned char buf[80*64*4];
	VtManager* m = makeMgr(buf);
	CHECK(m->switchTo(1));
	CHECK(m->active() == 1);
	CHECK(g_sigCount == 0);
}
