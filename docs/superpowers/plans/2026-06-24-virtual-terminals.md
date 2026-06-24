# Virtual Terminals (Ctrl+Alt+Fn) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Linux-style virtual consoles — 6 kernel text consoles (`Ctrl+Alt+F1..F6`) plus a graphics console (`Ctrl+Alt+F7`), with the VT machinery in the kernel, reusing the existing fbcon, PS/2 input, sessions and job control.

**Architecture:** A kernel `VtManager` owns N `VtConsole` objects and the real framebuffer; only the active VT blits. The PS/2 input path intercepts `Ctrl+Alt+Fn` and routes all other input to the active VT. `/dev/tty1..7` are CharDevices over the consoles; console fds bind to a VT. Text VTs render with fbcon; the graphics VT (F7) hands `/dev/fb0` to `nwm` via a Linux `VT_SETMODE`/`VT_RELDISP` acquire/release protocol. The PTY path (`/dev/ptmx`, `nterm`) is untouched.

**Tech Stack:** C++ freestanding (kernel), C (userland), doctest host tests (`make test64`), QEMU x86_64 MTTCG smokes (`make verify64`), build in Docker (`make image64`).

**Spec:** `docs/superpowers/specs/2026-06-24-virtual-terminals-design.md`

**Branch:** `feat/virtual-terminals` (already created; spec already committed).

---

## Conventions for every task

- **Build kernel:** `make image64`. **Host tests:** `make test64`. **Arch cleanliness:** `make check-arch`. **Full gate:** `make verify64`.
- Host tests live in `tests/*.cpp` (doctest, auto-globbed by `TEST_SRCS`). New MI modules under test go in `TEST_MODULES` in the `Makefile` (around line 2214–2241).
- Commits: **NO Claude/AI attribution**, no co-author trailer. Clean messages.
- TDD: failing test → run → minimal impl → pass → commit.
- Constant: never block / never touch the kernel heap from the keyboard IRQ. The VT switch is *requested* in IRQ context and the heavy part (graphics release-wait) runs in thread context — see Phase 5.

---

## File Structure

**New:**
- `kernel/vt/VtIoctl.h` — Linux `VT_*` / `KD*` ioctl numbers + `vt_mode`/`vt_stat` structs.
- `kernel/vt/VtConsole.h` / `.cpp` — one virtual console (input state + fbcon + fg pgrp + mode).
- `kernel/vt/VtManager.h` / `.cpp` — owns the consoles + active index + real framebuffer; `switchTo`.
- `drivers/VtTty.h` / `.cpp` — the `/dev/ttyN` CharDevice wrapping a `VtConsole`.
- `tests/test_fbconsole_live.cpp`, `tests/test_vt_console.cpp`, `tests/test_vt_switch.cpp`.
- `scripts/smoke-vt.sh`.

**Modified:**
- `drivers/FbConsole.h` / `.cpp` — add `live` bit + `repaintAll()`.
- `arch/x86_64/drivers/input_x86_64.cpp`, `arch/x86/drivers/input_x86.cpp` — move input singleton into the active `VtConsole`; add the `Ctrl+Alt+Fn` matcher; gate `/dev/input0`.
- `arch/x86_64/drivers/console_x86_64.cpp` — `consolePutChar` routes to the kernel-console VT; `VtManager` owns the LFB surface.
- `arch/include/arch/console.h`, `arch/include/arch/input.h` — declarations for the new entry points.
- `kernel/Syscall.h` / `.cpp` — bind `isConsole` fds to a VT; route read/write/termios/poll per-VT.
- `kernel/SyscallDispatch.cpp` — `VT_*`/`KD*`/`TIOCSCTTY` ioctls; per-VT pgrp/winsize.
- `kernel/Exec.cpp`, `kernel/SignalDispatch.h` — per-VT fg pgrp; `consoleSignal` → active VT.
- `kernel/Process.h` / `.cpp` — per-session controlling-tty (VT index).
- `kernel/Kernel.cpp` — construct `VtManager`; register `/dev/tty0..7`, `/dev/tty`, `/dev/console`.
- `user/init.c` — spawn `login` on tty1..6, `nwm` on tty7, per-VT respawn.
- `user/nwm/nwm.c` — `KDSETMODE`/`VT_SETMODE` + release/acquire handlers + `VT_RELDISP`.
- `Makefile` — `TEST_MODULES` additions; `smoke-vt` target wired into `verify64`.

**Shared constants:** `kVtCount = 7` (tty1..7), graphics VT index = 7. Define once in `kernel/vt/VtManager.h`.

---

## Phase 0 — `FbConsole`: off-screen rendering (`live` + `repaintAll`)

A `VtConsole` keeps its grid even when not visible. Add a `live` bit to `FbConsole`: when not live, feed the VT engine but skip pixel blits; `repaintAll()` blits the entire grid and re-syncs the shadow when a console becomes visible. Behavior is unchanged for the single existing console (it is constructed live).

### Task 0.1: `FbConsole` gains `live` + `repaintAll`

**Files:**
- Modify: `drivers/FbConsole.h`, `drivers/FbConsole.cpp`
- Test: `tests/test_fbconsole_live.cpp`

- [ ] **Step 1: Write the failing test**

`tests/test_fbconsole_live.cpp`:
```cpp
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
}
```

- [ ] **Step 2: Run it, watch it fail**

Add `drivers/FbConsole.cpp` is already in `TEST_MODULES`. Run:
`make test64`
Expected: compile error — `setLive`/`repaintAll` undeclared.

- [ ] **Step 3: Implement**

`drivers/FbConsole.h` — add to the class (after `m_curShown;`):
```cpp
	bool m_live = true;         // when false, putChar updates the grid but does not blit
```
and to the public methods (after `setCursor`):
```cpp
	void setLive(bool on) { m_live = on; }
	bool live() const { return m_live; }
	void repaintAll();          // blit the entire grid (used when a console becomes visible)
```

`drivers/FbConsole.cpp` — guard the blit calls and add `repaintAll`:
```cpp
void FbConsole::putChar(char c) {
	if (m_live) eraseCursor();
	vt_feed(&m_vt, (const unsigned char*) &c, 1);
	if (m_live) { renderDirty(); drawCursor(); }
}

// Becoming the visible console: the LFB shows some other VT's pixels, so the shadow is stale.
// Reset the shadow to "blank" and blit every grid cell, then draw the cursor.
void FbConsole::repaintAll() {
	m_live = true;
	fbFillRect(m_surf, 0, 0, m_surf.width, m_surf.height, m_bg);
	for (int y = 0; y < VT_MAXR; y++)
		for (int x = 0; x < VT_MAXC; x++)
			m_shadow[y][x] = vt_cell{};            // force every cell to differ
	for (int y = 0; y < m_vt.rows; y++) { renderRow(y); m_vt.dirty[y] = 0; }
	m_curShown = false;
	drawCursor();
}
```
Also guard `clear()` and `setCursor()` to no-op their blits when `!m_live`:
```cpp
void FbConsole::clear() {
	if (m_live) fbFillRect(m_surf, 0, 0, m_surf.width, m_surf.height, m_bg);
	const unsigned char seq[] = { 0x1b, '[', '2', 'J', 0x1b, '[', 'H' };
	vt_feed(&m_vt, seq, sizeof seq);
	for (int y = 0; y < VT_MAXR; y++) {
		for (int x = 0; x < VT_MAXC; x++) m_shadow[y][x] = m_vt.grid[y][x];
		m_vt.dirty[y] = 0;
	}
	m_curShown = false;
	if (m_live) drawCursor();
}

void FbConsole::setCursor(unsigned x, unsigned y) {
	if (m_live) eraseCursor();
	m_vt.cx = (m_vt.cols && (int) x >= m_vt.cols) ? m_vt.cols - 1 : (int) x;
	m_vt.cy = (m_vt.rows && (int) y >= m_vt.rows) ? m_vt.rows - 1 : (int) y;
	if (m_live) drawCursor();
}
```
(If `vt_cell{}` is not value-initializable in this C++ mode, instead set `m_shadow[y][x].ch = 0xFFFF; m_shadow[y][x].fg = m_shadow[y][x].bg = 0xFFFFFFFF;` — any value that cannot equal a real cell.)

- [ ] **Step 4: Run tests, watch pass**

`make test64`
Expected: PASS, all prior tests still green.

- [ ] **Step 5: Add the test to the suite (already auto-globbed) and commit**

```bash
git add drivers/FbConsole.h drivers/FbConsole.cpp tests/test_fbconsole_live.cpp
git commit -m "feat(fbcon): off-screen rendering — live flag + repaintAll"
```

---

## Phase 1 — VT core: `VtConsole` + `VtManager` (MI, host-tested)

The switching state machine, isolated from kernel wiring. Pure logic over a fake framebuffer + a fake signal sink, so it is fully host-tested before any IRQ/device integration.

### Task 1.1: `VtIoctl.h` — the Linux ABI constants

**Files:**
- Create: `kernel/vt/VtIoctl.h`

- [ ] **Step 1: Write the header** (no test — pure constants; exercised by later tasks)

```cpp
/*
 * VtIoctl.h — Linux virtual-terminal ioctl numbers + structs (the subset NanOS implements).
 * Values match Linux so userland (nwm) and any ported tool see the real ABI.
 */
#pragma once
#include <stdint.h>

// KD modes (console graphics/text), arg is an int.
#define KDGETMODE   0x4B3B
#define KDSETMODE   0x4B3A
#define KD_TEXT     0x00
#define KD_GRAPHICS 0x01

// VT switching control.
#define VT_OPENQRY   0x5600   // arg: int* -> first free VT number (or -1)
#define VT_GETMODE   0x5601   // arg: struct vt_mode*
#define VT_SETMODE   0x5602   // arg: struct vt_mode*
#define VT_GETSTATE  0x5603   // arg: struct vt_stat*
#define VT_ACTIVATE  0x5606   // arg: int (1-based VT number)
#define VT_WAITACTIVE 0x5607  // arg: int (1-based VT number)
#define VT_RELDISP   0x5605   // arg: int (1 = release granted, 2 = acquire ack)

#define VT_AUTO    0x00       // vt_mode.mode: kernel auto-switches
#define VT_PROCESS 0x01       // vt_mode.mode: process-controlled (relsig/acqsig)

struct vt_mode {
	uint8_t mode;     // VT_AUTO or VT_PROCESS
	uint8_t waitv;    // unused (0)
	int16_t relsig;   // signal sent to ask the owner to release
	int16_t acqsig;   // signal sent when the owner acquires
	int16_t frsig;    // unused (0)
};

struct vt_stat {
	uint16_t v_active;   // 1-based active VT
	uint16_t v_signal;   // unused (0)
	uint16_t v_state;    // bitmask of allocated VTs
};
```

- [ ] **Step 2: Commit**
```bash
git add kernel/vt/VtIoctl.h
git commit -m "feat(vt): Linux VT_*/KD* ioctl ABI constants"
```

### Task 1.2: `VtConsole` skeleton (mode + fg pgrp + fbcon, no input yet)

**Files:**
- Create: `kernel/vt/VtConsole.h`, `kernel/vt/VtConsole.cpp`
- Test: `tests/test_vt_console.cpp`
- Modify: `Makefile` (add `kernel/vt/VtConsole.cpp` to `TEST_MODULES`)

- [ ] **Step 1: Write the failing test**

`tests/test_vt_console.cpp`:
```cpp
#include "doctest.h"
#include "vt/VtConsole.h"
#include "vt/VtIoctl.h"
#include "Framebuffer.h"
using namespace kernel;

static FbSurface surf(unsigned char* b, int w, int h) {
    return FbSurface{ b, (unsigned)(w*4), (unsigned)w, (unsigned)h, 32 };
}

TEST_CASE("VtConsole starts as a text console with no fg pgrp and is created off-screen") {
    static unsigned char buf[8*10*16*4];
    VtConsole vt;
    vt.init(surf(buf, 80, 64), /*index=*/2);
    CHECK(vt.index() == 2);
    CHECK(vt.mode() == KD_TEXT);
    CHECK(vt.fgPgrp() == 0);
    CHECK_FALSE(vt.fbcon().live());        // VtManager makes exactly one live
}

TEST_CASE("VtConsole fg pgrp and KD mode are independently settable") {
    static unsigned char buf[8*10*16*4];
    VtConsole vt;
    vt.init(surf(buf, 80, 64), 7);
    vt.setFgPgrp(42);   CHECK(vt.fgPgrp() == 42);
    vt.setMode(KD_GRAPHICS); CHECK(vt.mode() == KD_GRAPHICS);
}
```

- [ ] **Step 2: Run, watch fail**

Add to `Makefile` `TEST_MODULES` (after the `drivers/...FbConsole.cpp...Pty.cpp` line ~2217):
```make
TEST_MODULES+= kernel/vt/VtConsole.cpp
```
`make test64` → fails: `vt/VtConsole.h` not found / class undefined.

Note: `HINCLUDES` (Makefile ~2204) already has `-Ikernel`, so `#include "vt/VtConsole.h"` resolves. Confirm `-Ikernel` is present (it is).

- [ ] **Step 3: Implement the skeleton**

`kernel/vt/VtConsole.h`:
```cpp
/*
 * VtConsole.h — one virtual console: an fbcon text grid + (Phase 2) input state +
 * foreground process group + KD_TEXT/KD_GRAPHICS mode + (Phase 5) VT_SETMODE state.
 * Machine-independent; the active console's input is fed by the arch layer.
 */
#pragma once
#include "FbConsole.h"
#include "Termios.h"
#include "LineDiscipline.h"
#include "KeyDecoder.h"
#include "WaitQueue.h"
#include "vt/VtIoctl.h"

namespace kernel {

class VtConsole {
	int      m_index = 0;        // 1-based VT number
	int      m_mode = KD_TEXT;   // KD_TEXT (kernel draws) or KD_GRAPHICS (owner draws via /dev/fb0)
	int      m_fgPgrp = 0;       // foreground process group (job control); 0 = none
	FbConsole m_fb;

	// --- input state (Phase 2): moved out of the arch singleton, now per-VT ---
	LineDiscipline m_line;
	KeyDecoder     m_decoder;
	bool           m_raw = false;
	WaitQueue      m_inputWq;
	static const int RAWCAP = 256;
	volatile unsigned char m_rawbuf[RAWCAP];
	volatile int   m_rawHead = 0, m_rawTail = 0;

	// --- VT_SETMODE state (Phase 5) ---
	int  m_vtMode = VT_AUTO;     // VT_AUTO or VT_PROCESS
	int  m_relsig = 0, m_acqsig = 0;
	int  m_ownerPid = 0;         // pid that did VT_SETMODE(VT_PROCESS)
	bool m_relWait = false;      // a release was requested, awaiting VT_RELDISP

	void rawPush(unsigned char b);
	bool rawEmpty() const { return m_rawHead == m_rawTail; }
	unsigned char rawPop();
public:
	void init(const FbSurface& s, int index);

	int  index() const { return m_index; }
	int  mode() const { return m_mode; }
	void setMode(int m) { m_mode = m; }
	int  fgPgrp() const { return m_fgPgrp; }
	void setFgPgrp(int p) { m_fgPgrp = p; }
	FbConsole& fbcon() { return m_fb; }

	// Termios / raw flag (per-VT; Phase 2/3).
	Termios& termios() { return m_termios; }
	bool raw() const { return m_raw; }
	void setRaw(bool r);
	Termios m_termios;          // public-ish via accessor; kept simple

	// Input (Phase 2): feed one decoded event; blocking read; readiness.
	void feedScancode(unsigned char sc);   // run decoder -> line discipline / raw ring, wake readers
	int  read(char* buf, unsigned n, bool nonblock);
	bool inputReady() const;

	// Output: write bytes to the fbcon (blits iff live).
	void write(const char* buf, unsigned n);

	// VT_SETMODE accessors (Phase 5).
	int  vtMode() const { return m_vtMode; }
	void setVtMode(int m, int rel, int acq, int owner) { m_vtMode = m; m_relsig = rel; m_acqsig = acq; m_ownerPid = owner; }
	int  relsig() const { return m_relsig; }
	int  acqsig() const { return m_acqsig; }
	int  ownerPid() const { return m_ownerPid; }
	bool relWait() const { return m_relWait; }
	void setRelWait(bool w) { m_relWait = w; }
};

}  // namespace kernel
```

`kernel/vt/VtConsole.cpp` — implement only what the Task 1.2 tests touch; the input bodies come in Phase 2 but compile now (they reference real types):
```cpp
#include "vt/VtConsole.h"
#include "Scheduler.h"
#include "SignalDispatch.h"
#include "Syscall.h"   // EAGAIN/ERESTARTSYS
#include "Console.h"

namespace kernel {

void VtConsole::init(const FbSurface& s, int index) {
	m_index = index;
	m_fb.init(s);
	m_fb.setLive(false);          // VtManager makes exactly one live
	initCookedTermios(m_termios);
}

void VtConsole::rawPush(unsigned char b) {
	int next = (m_rawHead + 1) % RAWCAP;
	if (next != m_rawTail) { m_rawbuf[m_rawHead] = b; m_rawHead = next; }
}
unsigned char VtConsole::rawPop() {
	unsigned char b = m_rawbuf[m_rawTail];
	m_rawTail = (m_rawTail + 1) % RAWCAP;
	return b;
}

void VtConsole::setRaw(bool r) {
	m_raw = r;
	m_rawHead = m_rawTail = 0;
	m_line = LineDiscipline();
	m_decoder = KeyDecoder();
}

void VtConsole::write(const char* buf, unsigned n) {
	for (unsigned i = 0; i < n; i++) m_fb.putChar(buf[i]);
}

// feedScancode / read / inputReady implemented in Phase 2.

}  // namespace kernel
```
For Task 1.2 to link in the host test, provide trivial Phase-2 stubs at the bottom of the `.cpp` (replaced in Phase 2):
```cpp
namespace kernel {
void VtConsole::feedScancode(unsigned char) {}
int  VtConsole::read(char*, unsigned, bool) { return 0; }
bool VtConsole::inputReady() const { return m_raw ? !rawEmpty() : false; }
}
```

- [ ] **Step 4: Run, watch pass**

`make test64` → PASS.

- [ ] **Step 5: Commit**
```bash
git add kernel/vt/VtConsole.h kernel/vt/VtConsole.cpp tests/test_vt_console.cpp Makefile
git commit -m "feat(vt): VtConsole skeleton — fbcon + mode + fg pgrp"
```

### Task 1.3: `VtManager` switching state machine (text VTs)

**Files:**
- Create: `kernel/vt/VtManager.h`, `kernel/vt/VtManager.cpp`
- Test: `tests/test_vt_switch.cpp`
- Modify: `Makefile` (`TEST_MODULES+= kernel/vt/VtManager.cpp`)

The graphics signalling is injected via a function pointer so the host test can observe it without a real scheduler/signals.

- [ ] **Step 1: Write the failing test**

`tests/test_vt_switch.cpp`:
```cpp
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
    // owner acks the release -> the pending switch completes
    m->relDisp(7, /*release granted*/1);
    CHECK(m->active() == 2);
    CHECK(m->vt(2)->fbcon().live());
    CHECK_FALSE(m->vt(7)->relWait());
}

TEST_CASE("switching to the already-active VT is a no-op") {
    static unsigned char buf[80*64*4];
    VtManager* m = makeMgr(buf);
    CHECK(m->switchTo(1));
    CHECK(m->active() == 1);
    CHECK(g_sigCount == 0);
}
```

- [ ] **Step 2: Run, watch fail**

Add to `Makefile` `TEST_MODULES`:
```make
TEST_MODULES+= kernel/vt/VtManager.cpp
```
`make test64` → fails: `VtManager` undefined.

- [ ] **Step 3: Implement**

`kernel/vt/VtManager.h`:
```cpp
/*
 * VtManager.h — owns the virtual consoles + the active index + the real framebuffer.
 * switchTo() drives the Linux VT discipline: text VTs flip a live bit and repaint; a
 * VT_PROCESS graphics VT is asked to release (relsig) and the switch completes only when
 * the owner acks via relDisp() (VT_RELDISP). Signalling is injected (SignalFn) so the core
 * is host-testable; the kernel wires it to the real per-pid signal sender.
 */
#pragma once
#include "vt/VtConsole.h"
#include "Framebuffer.h"

namespace kernel {

const int kVtCount = 7;        // tty1..tty7
const int kVtGraphics = 7;     // F7 is the graphics console

typedef void (*VtSignalFn)(int pid, int sig);

class VtManager {
	VtConsole  m_vt[kVtCount + 1];   // 1-based; index 0 unused
	int        m_active = 1;
	int        m_pending = 0;        // a switch awaiting VT_RELDISP (0 = none)
	FbSurface  m_surf{};
	VtSignalFn m_signal = 0;
public:
	void init(const FbSurface& s, VtSignalFn sig);
	int  active() const { return m_active; }
	VtConsole* vt(int n) { return (n >= 1 && n <= kVtCount) ? &m_vt[n] : 0; }
	VtConsole* activeVt() { return &m_vt[m_active]; }

	// Returns true if the switch completed synchronously; false if it is pending a release ack.
	bool switchTo(int n);
	// VT_RELDISP from the owner: release granted (arg 1) completes a pending switch.
	void relDisp(int n, int arg);
	int  openqry() const;                    // first free (always-allocated here) — returns -1 if none
};

extern VtManager* g_vtmgr;                   // the kernel's instance (null under host tests using a local)

}  // namespace kernel
```

`kernel/vt/VtManager.cpp`:
```cpp
#include "vt/VtManager.h"

namespace kernel {

VtManager* g_vtmgr = 0;

void VtManager::init(const FbSurface& s, VtSignalFn sig) {
	m_surf = s;
	m_signal = sig;
	for (int i = 1; i <= kVtCount; i++) m_vt[i].init(s, i);
	m_vt[kVtGraphics].setMode(KD_GRAPHICS);  // F7 is graphics by default
	m_active = 1;
	m_pending = 0;
	m_vt[1].fbcon().repaintAll();            // VT1 is the live console at boot
}

// Activate VT n (make it the live/owning console). Internal: assumes the previous owner
// has already released (no pending graphics ack outstanding).
static void acquire(VtConsole& v, VtSignalFn signal) {
	if (v.mode() == KD_GRAPHICS) {
		if (v.vtMode() == VT_PROCESS && signal && v.ownerPid())
			signal(v.ownerPid(), v.acqsig());   // owner redraws via /dev/fb0
		// kernel draws nothing on a graphics VT
	} else {
		v.fbcon().repaintAll();                 // text: blit the whole grid
	}
}

bool VtManager::switchTo(int n) {
	if (n < 1 || n > kVtCount || n == m_active) return true;
	VtConsole& cur = m_vt[m_active];
	// Releasing a process-mode graphics VT: ask the owner, park until VT_RELDISP.
	if (cur.mode() == KD_GRAPHICS && cur.vtMode() == VT_PROCESS && m_signal && cur.ownerPid()) {
		cur.setRelWait(true);
		m_pending = n;
		m_signal(cur.ownerPid(), cur.relsig());
		return false;                            // completes in relDisp()
	}
	// Text VT (or auto-mode graphics): release is immediate.
	cur.fbcon().setLive(false);
	m_active = n;
	acquire(m_vt[n], m_signal);
	return true;
}

void VtManager::relDisp(int n, int arg) {
	VtConsole& v = m_vt[n];
	if (!v.relWait() || m_pending == 0) return;
	v.setRelWait(false);
	if (arg != 1) { m_pending = 0; return; }     // owner refused the release
	int target = m_pending;
	m_pending = 0;
	// the graphics owner has released; now switch
	m_active = target;
	acquire(m_vt[target], m_signal);
}

int VtManager::openqry() const {
	return -1;   // all VTs pre-allocated; nothing "free" to hand out
}

}  // namespace kernel
```

- [ ] **Step 4: Run, watch pass**

`make test64` → PASS (all five new cases + existing suite).

- [ ] **Step 5: Commit**
```bash
git add kernel/vt/VtManager.h kernel/vt/VtManager.cpp tests/test_vt_switch.cpp Makefile
git commit -m "feat(vt): VtManager switching state machine (text + graphics release/acquire)"
```

---

## Phase 2 — Per-VT input + `Ctrl+Alt+Fn` intercept + kernel wiring

Move the input singleton into `VtConsole`, intercept the switch combo, route to the active VT, and wire `VtManager` into the kernel so `consolePutChar` reaches VT1.

### Task 2.1: `VtConsole` input bodies (host-tested)

**Files:**
- Modify: `kernel/vt/VtConsole.cpp` (replace the Phase-1 stubs)
- Test: extend `tests/test_vt_console.cpp`

The control-key→signal calls go through a tiny injected hook so the test can observe them without the real signal subsystem.

- [ ] **Step 1: Write the failing test** (append to `tests/test_vt_console.cpp`)
```cpp
TEST_CASE("raw-mode VtConsole buffers decoded bytes and read() drains them") {
    static unsigned char buf[8*10*16*4];
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
    static unsigned char buf[8*10*16*4];
    VtConsole vt; vt.init(surf(buf, 80, 64), 1);
    vt.setRaw(true);
    char out[8];
    CHECK(vt.read(out, sizeof out, true) == -kernel::EAGAIN);
}
```

- [ ] **Step 2: Run, watch fail** — `make test64` (read returns 0, not 2 / not -EAGAIN).

- [ ] **Step 3: Implement** — replace the stub block in `kernel/vt/VtConsole.cpp` with the real bodies, lifted from `arch/x86_64/drivers/input_x86_64.cpp` but operating on members. Control keys call `consoleSignalGroup(sig, m_fgPgrp)` (job control per-VT):
```cpp
namespace {
void echoChar(VtConsole* v, char c);   // fwd
}

void VtConsole::feedScancode(unsigned char sc) {
	if (m_raw) {
		m_decoder.feed(sc, [this](int ev) {
			if (ev < 256) rawPush((unsigned char) ev);
			else { rawPush(0x1B); rawPush('['); rawPush(
				ev == KEY_UP ? 'A' : ev == KEY_DOWN ? 'B' : ev == KEY_RIGHT ? 'C' : 'D'); }
		});
	} else {
		m_decoder.feed(sc, [this](int ev) {
			if (ev == 0x03) { consoleSignalGroup(SIGINT,  m_fgPgrp); return; }
			if (ev == 0x1C) { consoleSignalGroup(SIGQUIT, m_fgPgrp); return; }
			if (ev == 0x1A) { consoleSignalGroup(SIGTSTP, m_fgPgrp); return; }
			auto push = [this](char c){ m_line.push(c, [this](char e){ m_fb.putChar(e); }); };
			if (ev < 256) push((char) ev);
			else { push(0x1B); push('['); push(
				ev == KEY_UP ? 'A' : ev == KEY_DOWN ? 'B' : ev == KEY_RIGHT ? 'C' : 'D'); }
		});
	}
	if ((m_raw && !rawEmpty()) || (!m_raw && m_line.lineReady()))
		Scheduler::wakeAll(&m_inputWq);
}

int VtConsole::read(char* buf, unsigned n, bool nonblock) {
	if (m_raw) {
		if (nonblock && rawEmpty()) return -EAGAIN;
		while (rawEmpty()) {
			Scheduler::sleepOn(&m_inputWq);
			if (hasPendingSignalCurrent()) return -ERESTARTSYS;
		}
		unsigned i = 0; while (i < n && !rawEmpty()) buf[i++] = (char) rawPop();
		return (int) i;
	}
	if (nonblock && !m_line.lineReady()) return -EAGAIN;
	while (!m_line.lineReady()) {
		Scheduler::sleepOn(&m_inputWq);
		if (hasPendingSignalCurrent()) return -ERESTARTSYS;
	}
	return m_line.takeLine(buf, (int) n);
}

bool VtConsole::inputReady() const {
	return m_raw ? !rawEmpty() : const_cast<LineDiscipline&>(m_line).lineReady();
}
```
(Note: the existing `input_x86_64.cpp` uses `sleepOnUntil(&wq, pred, 0)` to avoid lost wakeups; keep that exact pattern here — pass small static predicate functions that re-test `rawEmpty()`/`lineReady()` on `this` via a captured pointer stored in a member, OR keep the proven `sleepOnUntil` signature. Match whichever `Scheduler` API the current input layer uses verbatim to preserve the lost-wakeup guarantee. The host shim makes `sleepOn*` a no-op, so the nonblock test path is what the host exercises.)

The host test never blocks (it uses `nonblock=true`), so the blocking path is validated later in QEMU. The host shim already stubs `Scheduler` sleep + `hasPendingSignalCurrent` (see `tests/host_shims.cpp`).

- [ ] **Step 4: Run, watch pass** — `make test64`.

- [ ] **Step 5: Commit**
```bash
git add kernel/vt/VtConsole.cpp tests/test_vt_console.cpp
git commit -m "feat(vt): per-VT input — line discipline, raw ring, job-control keys"
```

### Task 2.2: `Ctrl+Alt+Fn` matcher + route to active VT (arch input layer)

**Files:**
- Modify: `arch/x86_64/drivers/input_x86_64.cpp`, `arch/x86/drivers/input_x86.cpp` (parity)
- Modify: `arch/include/arch/input.h`
- Modify: `kernel/vt/VtManager.h`/`VtManager.cpp` (add `requestSwitch` from IRQ — see Phase 5; for now `switchTo` for text is synchronous and IRQ-safe)

This is MD code (arch input), so no host test; it is validated by the QEMU smoke (Phase 6). The matcher is a small, self-contained function.

- [ ] **Step 1: Add the matcher + reroute in `arch/x86_64/drivers/input_x86_64.cpp`**

Replace the singletons (`g_line`, `g_decoder`, `g_raw`, raw ring, `g_inputWq`) usage: `inputFeedScancode` now (a) runs the VT-switch matcher, (b) otherwise feeds the active `VtConsole`. Add at the top:
```cpp
#include "vt/VtManager.h"

namespace {
// Minimal Ctrl+Alt+Fn matcher, independent of the per-VT KeyDecoder (which doesn't track Alt).
bool g_ctrl = false, g_alt = false;
// Returns 1..7 if this scancode completes Ctrl+Alt+F1..F7, else 0. Tracks make/break of Ctrl/Alt.
int vtSwitchMatch(unsigned char sc) {
	static bool ext = false;
	if (sc == 0xE0) { ext = true; return 0; }
	bool brk = sc & 0x80; unsigned char code = sc & 0x7F;
	if (ext) { ext = false; if (code == 0x38) { g_alt = !brk; } if (code == 0x1D) { g_ctrl = !brk; } return 0; }
	if (code == 0x38) { g_alt = !brk; return 0; }       // left Alt
	if (code == 0x1D) { g_ctrl = !brk; return 0; }       // left Ctrl
	if (!brk && g_ctrl && g_alt && code >= 0x3B && code <= 0x41)
		return (int)(code - 0x3B) + 1;                   // F1..F7 -> 1..7
	return 0;
}
}
```
Rewrite `inputFeedScancode`:
```cpp
void inputFeedScancode(unsigned char sc) {
	int target = vtSwitchMatch(sc);
	if (target) { if (kernel::g_vtmgr) kernel::g_vtmgr->requestSwitch(target); return; }  // consumed
	kernel::VtConsole* v = kernel::g_vtmgr ? kernel::g_vtmgr->activeVt() : 0;
	if (!v) return;
	// /dev/input0 (evdev) only for the active VT, so a backgrounded graphics app stops seeing keys.
	kernel::kbdFeed(sc);
	v->feedScancode(sc);
}
```
Replace `inputRead`/`inputSetRaw`/`inputReady` with thin forwarders to a *specified* VT. Since the existing arch contract is parameterless (`arch::inputRead(buf,n,nonblock)`), keep that signature but operate on the **caller's controlling VT**, resolved by the syscall layer (Phase 3). For Phase 2, forward to the active VT to keep the build working:
```cpp
int  inputRead(char* b, unsigned n, int nb) { return kernel::g_vtmgr ? kernel::g_vtmgr->activeVt()->read(b, n, nb) : 0; }
void inputSetRaw(int raw) { if (kernel::g_vtmgr) kernel::g_vtmgr->activeVt()->setRaw(raw != 0); }
bool inputReady() { return kernel::g_vtmgr ? kernel::g_vtmgr->activeVt()->inputReady() : false; }
```
(Phase 3 replaces "active VT" with "the fd's bound VT".)

- [ ] **Step 2: Add `requestSwitch` to `VtManager`** (IRQ-safe for text; defers graphics)

`VtManager.h`: add `void requestSwitch(int n);` and a `volatile int m_irqPending = 0;` + `void servicePending();`.
`VtManager.cpp`:
```cpp
// Called from the keyboard IRQ. Text switches are flag-flips + repaint (IRQ-safe). A switch
// that must release a process-mode graphics VT cannot run in IRQ context (it signals + waits),
// so it is deferred: record it and let the next thread-context tick service it.
void VtManager::requestSwitch(int n) {
	if (n < 1 || n > kVtCount || n == m_active) return;
	VtConsole& cur = m_vt[m_active];
	bool needsRelease = cur.mode() == KD_GRAPHICS && cur.vtMode() == VT_PROCESS && cur.ownerPid();
	if (needsRelease) { m_irqPending = n; return; }   // thread context will run switchTo()
	switchTo(n);                                       // text: safe now
}
void VtManager::servicePending() {
	int n = m_irqPending; if (!n) return; m_irqPending = 0; switchTo(n);
}
```
Call `servicePending()` from a thread-context hook — the scheduler tick or the net/idle path. Add a call in `Scheduler::onTick` (thread context) guarded by `if (g_vtmgr) g_vtmgr->servicePending();`. (Confirm `onTick` runs in thread context in this scheduler — the deferred-preemption model runs the body in thread context; see the scheduler memory note.)

- [ ] **Step 3: Mirror into `arch/x86/drivers/input_x86.cpp`** with the identical matcher + forwarders (i686 parity, so `make image` for x86 still builds).

- [ ] **Step 4: Build** — `make image64` and `make check-arch`.
Expected: builds; `check-arch` clean (VtManager is MI; only the matcher lives in MD arch input, which is allowed).

- [ ] **Step 5: Commit**
```bash
git add arch/x86_64/drivers/input_x86_64.cpp arch/x86/drivers/input_x86.cpp arch/include/arch/input.h kernel/vt/VtManager.h kernel/vt/VtManager.cpp kernel/Scheduler.cpp
git commit -m "feat(vt): intercept Ctrl+Alt+Fn and route input to the active console"
```

### Task 2.3: Construct `VtManager`; route `consolePutChar` to the kernel-console VT

**Files:**
- Modify: `arch/x86_64/drivers/console_x86_64.cpp`, `arch/include/arch/console.h`
- Modify: `kernel/Kernel.cpp`

- [ ] **Step 1: Add a kernel-console output entry that targets VT1**

`arch/include/arch/console.h`: declare `void consoleAttachVt();` (called once after `VtManager` is built).
`console_x86_64.cpp`: after `consoleActivateFramebuffer`, once VTs exist, `consolePutChar` should write to VT1's fbcon (the kernel console), not the bare `g_fb`. Implement by having `VtManager::init` take over the same `FbSurface` and pointing the kernel console at `g_vtmgr->vt(1)->fbcon()`:
```cpp
void consolePutChar(char c) {
	kernel::RecursiveIrqGuard g(g_consoleLock);
	if (c == '\n') serialPut('\r');
	serialPut(c);
	if (kernel::g_vtmgr) {                       // VTs up: kernel console = VT1, drawn iff VT1 active
		kernel::g_vtmgr->vt(1)->fbcon().putChar(c);
		return;
	}
	if (g_useFb) { g_fb.putChar(c); return; }
	/* ...existing VGA path... */
}
```
Apply the same `g_vtmgr` check in `consoleClear`/`consoleSetCursor`/`consoleSize` (delegate to `vt(1)->fbcon()`).

- [ ] **Step 2: Build `VtManager` in `Kernel.cpp`**

After `consoleActivateFramebuffer()` runs (framebuffer mapped) and before the `/dev` nodes are registered, add:
```cpp
// Virtual terminals: build the manager over the bootloader framebuffer. The per-pid signal
// sender (for the graphics VT's release/acquire) is the real signal subsystem.
okBegin("Virtual terminals (tty1..tty7)");
const arch::BootFramebuffer* fbv = arch::bootFramebuffer();
if (fbv) {
	kernel::FbSurface s = { (uint8_t*)(uintptr_t) fbv->addr, fbv->pitch, fbv->width, fbv->height, fbv->bpp };
	static kernel::VtManager mgr;
	mgr.init(s, [](int pid, int sig){ kernel::signalSendPid(pid, sig); });
	kernel::g_vtmgr = &mgr;
}
okEnd();
```
(`signalSendPid` — confirm the exact per-pid send helper name in `kernel/Signal*`/`Exec.cpp`; it is the function `kill(2)` uses. If it is `signalSend(pid,sig)` use that.)

- [ ] **Step 3: Build + boot smoke** — `make image64` then a quick manual `make run` (or the existing boot smoke). Expected: boots to the tty1 login exactly as before (single console; VT1 is live). No visible change yet — switching has no second login until Phase 4, but `Ctrl+Alt+F2` should show a blank VT2 (cleared grid) and `Ctrl+Alt+F1` returns to the shell.

- [ ] **Step 4: `make check-arch`** — clean.

- [ ] **Step 5: Commit**
```bash
git add arch/x86_64/drivers/console_x86_64.cpp arch/include/arch/console.h kernel/Kernel.cpp
git commit -m "feat(vt): construct VtManager; route kernel console to VT1"
```

---

## Phase 3 — Device nodes + per-VT console fds + ioctls + job control

### Task 3.1: `VtTty` CharDevice over a `VtConsole`

**Files:**
- Create: `drivers/VtTty.h`, `drivers/VtTty.cpp`
- Modify: `Makefile` (`TEST_MODULES+= drivers/VtTty.cpp`)
- Test: `tests/test_vttty.cpp`

- [ ] **Step 1: Write the failing test**
```cpp
#include "doctest.h"
#include "VtTty.h"
#include "vt/VtManager.h"
#include "vt/VtIoctl.h"
using namespace kernel;

TEST_CASE("VtTty write goes to its VT's grid; TIOCSPGRP/TIOCGPGRP set the VT fg pgrp") {
    static unsigned char buf[80*64*4];
    static VtManager m; FbSurface s{buf,80*4,80,64,32}; m.init(s, 0); g_vtmgr = &m;
    VtTty tty(2);                              // /dev/tty2
    int pg = 55; CHECK(tty.ioctl(IOCTL_TIOCSPGRP, &pg) == 0);
    int got = 0; CHECK(tty.ioctl(IOCTL_TIOCGPGRP, &got) == 0);
    CHECK(got == 55);
    CHECK(m.vt(2)->fgPgrp() == 55);
}

TEST_CASE("VT_ACTIVATE via ioctl switches the active console; VT_GETSTATE reports it") {
    static unsigned char buf[80*64*4];
    static VtManager m; FbSurface s{buf,80*4,80,64,32}; m.init(s, 0); g_vtmgr = &m;
    VtTty tty(1);
    int n = 4; CHECK(tty.ioctl(VT_ACTIVATE, &n) == 0);
    CHECK(m.active() == 4);
    vt_stat st; CHECK(tty.ioctl(VT_GETSTATE, &st) == 0);
    CHECK(st.v_active == 4);
}
```

- [ ] **Step 2: Run, watch fail.**

- [ ] **Step 3: Implement** `drivers/VtTty.h`/`.cpp`. `read`/`write` delegate to `g_vtmgr->vt(m_index)`. `ioctl` handles `TIOCGPGRP/TIOCSPGRP` (VT fg pgrp), `TCGETS/TCSETS` (VT termios + `setRaw` per `ICANON`), `TIOCGWINSZ`, `VT_ACTIVATE`/`VT_WAITACTIVE`/`VT_GETSTATE`/`VT_OPENQRY`, `KDSETMODE`/`KDGETMODE`, `VT_SETMODE`/`VT_GETMODE`/`VT_RELDISP` (Phase 5 owner), `TIOCSCTTY` (Phase 3.3). `waitQueue()` returns the VT's input wait queue (add an accessor on `VtConsole`). `read` blocks via that queue.
```cpp
#pragma once
#include "CharDevice.h"
namespace kernel {
class VtTty : public CharDevice {
	int m_index;
public:
	explicit VtTty(int index) : m_index(index) {}
	int read(unsigned, void* buf, unsigned n) override;
	int write(unsigned, const void* buf, unsigned n) override;
	int ioctl(unsigned cmd, void* arg) override;
	int mmapInfo(uint64_t*, unsigned*) override { return -1; }
	short pollReady(short events) override;
	WaitQueue* waitQueue() override;
};
}
```
`.cpp` (key parts):
```cpp
#include "VtTty.h"
#include "vt/VtManager.h"
#include "vt/VtIoctl.h"
#include "Termios.h"
#include "Syscall.h"   // IOCTL_* + Winsize + errno

namespace kernel {
int VtTty::read(unsigned, void* buf, unsigned n) {
	VtConsole* v = g_vtmgr ? g_vtmgr->vt(m_index) : 0; if (!v) return -EIO;
	return v->read((char*) buf, n, false);
}
int VtTty::write(unsigned, const void* buf, unsigned n) {
	VtConsole* v = g_vtmgr ? g_vtmgr->vt(m_index) : 0; if (!v) return -EIO;
	v->write((const char*) buf, n); return (int) n;
}
int VtTty::ioctl(unsigned cmd, void* arg) {
	VtConsole* v = g_vtmgr ? g_vtmgr->vt(m_index) : 0; if (!v) return -EIO;
	switch (cmd) {
	case IOCTL_TIOCGPGRP: if (!arg) return -EINVAL; *(int*) arg = v->fgPgrp(); return 0;
	case IOCTL_TIOCSPGRP: if (!arg) return -EINVAL; v->setFgPgrp(*(int*) arg); return 0;
	case IOCTL_TCGETS:    if (!arg) return -EINVAL; *(Termios*) arg = v->termios(); return 0;
	case IOCTL_TCSETS: case IOCTL_TCSETSW: case IOCTL_TCSETSF:
		if (!arg) return -EINVAL; v->termios() = *(const Termios*) arg;
		v->setRaw((v->termios().c_lflag & TL_ICANON) == 0); return 0;
	case IOCTL_TIOCGWINSZ: { if (!arg) return -EINVAL; Winsize* ws = (Winsize*) arg;
		ws->ws_col = (unsigned short) v->fbcon().cols(); ws->ws_row = (unsigned short) v->fbcon().rows();
		ws->ws_xpixel = ws->ws_ypixel = 0; return 0; }
	case VT_ACTIVATE:    if (!arg) return -EINVAL; g_vtmgr->switchTo(*(int*) arg); return 0;
	case VT_WAITACTIVE:  return 0;   // Phase 5 makes this block until active
	case VT_GETSTATE: { if (!arg) return -EINVAL; vt_stat* st = (vt_stat*) arg;
		st->v_active = (unsigned short) g_vtmgr->active(); st->v_signal = 0;
		st->v_state = 0; for (int i = 1; i <= kVtCount; i++) st->v_state |= (1u << i); return 0; }
	case VT_OPENQRY:     if (!arg) return -EINVAL; *(int*) arg = g_vtmgr->openqry(); return 0;
	case KDGETMODE:      if (!arg) return -EINVAL; *(int*) arg = v->mode(); return 0;
	case KDSETMODE:      if (!arg) return -EINVAL; v->setMode(*(int*) arg); return 0;
	// VT_SETMODE / VT_GETMODE / VT_RELDISP / TIOCSCTTY: Phase 5 / Task 3.3.
	default: return -EINVAL;
	}
}
short VtTty::pollReady(short events) {
	VtConsole* v = g_vtmgr ? g_vtmgr->vt(m_index) : 0; short r = 0;
	if (v && (events & POLLIN) && v->inputReady()) r |= POLLIN;
	if (events & POLLOUT) r |= POLLOUT; return r;
}
WaitQueue* VtTty::waitQueue() {
	VtConsole* v = g_vtmgr ? g_vtmgr->vt(m_index) : 0; return v ? v->inputWaitQueue() : 0;
}
}
```
Add `WaitQueue* inputWaitQueue() { return &m_inputWq; }` to `VtConsole.h`.

- [ ] **Step 4: Run, watch pass** — `make test64`.

- [ ] **Step 5: Commit**
```bash
git add drivers/VtTty.h drivers/VtTty.cpp tests/test_vttty.cpp kernel/vt/VtConsole.h Makefile
git commit -m "feat(vt): /dev/ttyN CharDevice with VT_/KD ioctls + per-VT job control"
```

### Task 3.2: Register `/dev/tty0..7`, `/dev/tty`, `/dev/console`; per-VT pgrp in dispatch

**Files:**
- Modify: `kernel/Kernel.cpp`, `kernel/SyscallDispatch.cpp`, `kernel/Exec.cpp`, `kernel/SignalDispatch.h`

- [ ] **Step 1:** In `Kernel.cpp` `/dev` setup, after the `input0` node, register the VT devices:
```cpp
for (int i = 1; i <= kernel::kVtCount; i++) {
	char name[8] = { 't','t','y', (char)('0'+i), 0 };   // tty1..tty7 (i<=7)
	root->addChar(root->dev(), name, new kernel::VtTty(i), 0620);
}
root->addChar(root->dev(), "tty0", new kernel::VtTty(0 /*active proxy*/), 0620);  // see note
root->addChar(root->dev(), "console", new kernel::VtTty(1), 0600);
```
For `/dev/tty0` (active) and `/dev/tty` (controlling), `VtTty(0)` resolves `m_index==0` to `g_vtmgr->active()` at each call; add that resolution at the top of every `VtTty` method:
```cpp
int idx = m_index ? m_index : (g_vtmgr ? g_vtmgr->active() : 1);
VtConsole* v = g_vtmgr ? g_vtmgr->vt(idx) : 0;
```
`/dev/tty` (controlling terminal) uses the caller's controlling VT (Task 3.3); register it with a distinct sentinel `VtTty(-1)` that resolves via `ProcTable::current()` ctty.

- [ ] **Step 2:** Make `consoleSignal`/`consoleGetPgrp`/`consoleSetPgrp` (in `Exec.cpp`) operate on the **active VT** instead of the global `g_consolePgrp`:
```cpp
void consoleSetPgrp(int pgrp) { if (kernel::g_vtmgr) kernel::g_vtmgr->activeVt()->setFgPgrp(pgrp); }
int  consoleGetPgrp()         { return kernel::g_vtmgr ? kernel::g_vtmgr->activeVt()->fgPgrp() : 0; }
void consoleSignal(int sig)   { int pg = consoleGetPgrp(); if (pg > 0) signalSendGroup(pg, sig); /* fallback unchanged */ }
```
Keep `g_consolePgrp` only as the pre-VT fallback (before `g_vtmgr` exists).

- [ ] **Step 3:** In `SyscallDispatch.cpp` `SYS_ioctl`, the existing console `isConsoleFd` branches for `TIOCGPGRP/TIOCSPGRP`/`TIOCGWINSZ`/`TCSETS` already call `consoleGetPgrp`/`consoleSetPgrp`/`arch::consoleSize`/`arch::inputSetRaw` — these now resolve per-active-VT automatically. Add `TIOCSCTTY` and the `VT_*`/`KD*` passthrough for console fds by delegating to the bound VT's device (Task 3.3 binds the fd → VtTty so the generic `g_sys->ioctl` path reaches `VtTty::ioctl`). Verify no double-handling: remove the special-cased console pgrp/winsize block IF the fd now routes to a `VtTty` device (cleaner); otherwise keep it. Choose one: **route console fds through `VtTty`** (preferred — single source of truth).

- [ ] **Step 4: Build + boot** — `make image64`; boot; `cat /proc/...`? Just confirm boot reaches tty1 login and `Ctrl+Alt+F2` shows a blank console. `make check-arch`.

- [ ] **Step 5: Commit**
```bash
git add kernel/Kernel.cpp kernel/SyscallDispatch.cpp kernel/Exec.cpp kernel/SignalDispatch.h
git commit -m "feat(vt): register /dev/tty0..7,tty,console; per-VT job control in dispatch"
```

### Task 3.3: Bind console fds to a VT; controlling-tty (`TIOCSCTTY`, `/dev/tty`)

**Files:**
- Modify: `kernel/Syscall.h`, `kernel/Syscall.cpp`, `kernel/Process.h`, `kernel/Process.cpp`
- Test: extend `tests/test_syscall*.cpp` (an existing syscall test file) with a binding check

- [ ] **Step 1:** Add a per-session controlling-tty (VT index) to `Process` (`kernel/Process.h`): `int cttyVt = 0;` (0 = none). Setter/getter on `ProcTable` keyed by session leader. `TIOCSCTTY` on a `VtTty(n)` sets the caller's session `cttyVt = n`.

- [ ] **Step 2:** `Syscall.h`/`.cpp`: the `isConsole` fd gains `int vt;` (which VT it reads/writes). Default fds 0/1/2 set `vt = 1`. When a process `open`s `/dev/ttyN` the normal device path already returns a device fd (not `isConsole`) — so console reads of that fd go through `VtTty::read` (correct, blocks on that VT's queue). The `isConsole` fast-path remains only for the *default* inherited 0/1/2 with `vt=1`; its `read`/`write` route to `g_vtmgr->vt(fds[fd].vt)` instead of `arch::inputRead`/`consoleWrite`:
```cpp
// Syscall.cpp read():
if (fds[fd].isConsole) {
	VtConsole* v = g_vtmgr ? g_vtmgr->vt(fds[fd].vt) : 0;
	if (v) return v->read((char*) buf, n, (fds[fd].flags & O_NONBLOCK) != 0);
	return arch::inputRead((char*) buf, n, ...);   // pre-VT fallback
}
// write(): v->write(...) ; return n;
```
(Simplest end state: init opens `/dev/ttyN` explicitly and dups to 0/1/2, so children never use the `isConsole` fast-path. The fast-path then only matters for PID 1 before it forks. Keep the per-`vt` field for that case.)

- [ ] **Step 3:** `/dev/tty` resolution: `VtTty(-1)` resolves `idx = ProcTable::current()->session-ctty`. If none, `-ENXIO`.

- [ ] **Step 4: Build + host tests + boot** — `make test64`, `make image64`, `make check-arch`.

- [ ] **Step 5: Commit**
```bash
git add kernel/Syscall.h kernel/Syscall.cpp kernel/Process.h kernel/Process.cpp drivers/VtTty.cpp tests/test_syscall_console.cpp
git commit -m "feat(vt): controlling tty (TIOCSCTTY, /dev/tty) + per-fd VT binding"
```

---

## Phase 4 — init: a login per text console; printk/panic

### Task 4.1: init spawns `login` on tty1..tty6

**Files:**
- Modify: `user/init.c`

- [ ] **Step 1:** Refactor init's single-console respawn loop into a per-console getty. Replace the single `for(;;)` shell loop with: spawn a getty child per text VT (1..6), each: `setsid()`, `open("/dev/ttyN", O_RDWR)`, `ioctl(fd, TIOCSCTTY, 0)`, `dup2(fd,0/1/2)`, `tcsetpgrp(0, getpid())`, exec the same `login` path as today. The parent tracks the 6 child pids and respawns whichever exits (getty-style), plus reaps dropbear/orphans.
```c
#define NVT 6
static int vt_pid[NVT+1];
static void spawn_getty(int n) {
	int pid = fork();
	if (pid == 0) {
		setsid();
		char dev[12]; dev[0]='/';dev[1]='d';dev[2]='e';dev[3]='v';dev[4]='/';
		dev[5]='t';dev[6]='t';dev[7]='y';dev[8]=(char)('0'+n);dev[9]=0;
		int fd = open(dev, O_RDWR);
		if (fd < 0) _exit(127);
		ioctl(fd, 0x540E /*TIOCSCTTY*/, 0);
		dup2(fd,0); dup2(fd,1); dup2(fd,2); if (fd>2) close(fd);
		tcsetpgrp(0, getpid());
		char* largv[] = { (char*)"login", 0 };
		execve("/disks/main/nanos/bin/login.nxe", largv, environ);
		char* fb[] = { (char*)"nsh", 0 }; execve(FALLBACK_SHELL, fb, environ);
		_exit(127);
	}
	if (pid > 0) vt_pid[n] = pid;
}
```
Main loop: `for (n=1..6) spawn_getty(n);` then reap loop: on a reaped pid that equals some `vt_pid[n]`, `spawn_getty(n)` again. (Keep `SIGTTOU/SIGTTIN` ignored, DHCP/services as-is, the boot log redirect.)

- [ ] **Step 2: Build + boot** — `make image64`; boot. Expected: tty1 shows a login; `Ctrl+Alt+F2` shows a *second independent* login; logging in on each gives separate shells; `Ctrl+Alt+F1`..`F6` switch between them and each preserves its screen.

- [ ] **Step 3:** Confirm `Ctrl+C` only signals the foreground job of the *active* VT (job control is per-VT now).

- [ ] **Step 4: `make check-arch`** — clean.

- [ ] **Step 5: Commit**
```bash
git add user/init.c
git commit -m "feat(vt): init runs a login getty on tty1..tty6"
```

### Task 4.2: Panic force-switches to a text VT

**Files:**
- Modify: the panic path (`kernel/Kernel.cpp` `panic`/`okBegin` area or wherever `panic()` lives) + `arch/x86_64/...` panic print.

- [ ] **Step 1:** At the top of `panic()`, before printing: `if (kernel::g_vtmgr && kernel::g_vtmgr->active() != 1) kernel::g_vtmgr->switchTo(1);` and force VT1 to `KD_TEXT`/live so the panic text is visible even if F7 (graphics) was active.

- [ ] **Step 2: Build** — `make image64`. (Validated by reading the code; a deliberate panic test is optional.)

- [ ] **Step 3: Commit**
```bash
git add kernel/Kernel.cpp
git commit -m "feat(vt): panic switches to the text console so the message is visible"
```

---

## Phase 5 — Graphics VT (F7) + nwm handoff

### Task 5.1: `VT_SETMODE`/`VT_RELDISP` owner protocol + deferred release-wait

**Files:**
- Modify: `drivers/VtTty.cpp` (ioctls), `kernel/vt/VtManager.{h,cpp}`, `kernel/vt/VtConsole.h`
- Test: extend `tests/test_vt_switch.cpp`

- [ ] **Step 1: Write the failing test** (append) — a `VT_SETMODE` owner that does not ack within the timeout is taken over anyway:
```cpp
TEST_CASE("release ack timeout takes over the graphics VT anyway") {
    static unsigned char buf[80*64*4];
    VtManager* m = makeMgr(buf);
    m->vt(7)->setMode(KD_GRAPHICS);
    m->vt(7)->setVtMode(VT_PROCESS, 10, 12, 99);
    m->switchTo(7);
    CHECK_FALSE(m->switchTo(2));        // pending
    m->releaseTimeoutTick(); m->releaseTimeoutTick(); m->releaseTimeoutTick();  // exceed the deadline
    CHECK(m->active() == 2);            // forced over
}
```

- [ ] **Step 2: Implement** — add `VT_SETMODE`/`VT_GETMODE`/`VT_RELDISP` to `VtTty::ioctl`:
```cpp
case VT_SETMODE: { if (!arg) return -EINVAL; vt_mode* vm = (vt_mode*) arg;
	v->setVtMode(vm->mode, vm->relsig, vm->acqsig, vm->mode == VT_PROCESS ? currentPid() : 0); return 0; }
case VT_GETMODE: { if (!arg) return -EINVAL; vt_mode* vm = (vt_mode*) arg;
	vm->mode = (uint8_t) v->vtMode(); vm->relsig = (int16_t) v->relsig();
	vm->acqsig = (int16_t) v->acqsig(); vm->waitv = vm->frsig = 0; return 0; }
case VT_RELDISP: if (g_vtmgr) g_vtmgr->relDisp(m_index ? m_index : g_vtmgr->active(), (int)(long) arg); return 0;
```
(`VT_RELDISP`'s arg is passed by value in Linux; the dispatch must pass the raw int, not a pointer — handle in `SyscallDispatch` so `arg` carries the integer.)

Add a release deadline to `VtManager`: a tick counter set when `requestSwitch` parks on a graphics release, decremented by `servicePending()` each thread-context tick; on expiry force the switch (`m_vt[active].setRelWait(false); m_active = m_pending; acquire(...)`). Expose `releaseTimeoutTick()` for the test.

- [ ] **Step 3: Run, watch pass** — `make test64`.

- [ ] **Step 4: Build** — `make image64`, `make check-arch`.

- [ ] **Step 5: Commit**
```bash
git add drivers/VtTty.cpp kernel/vt/VtManager.h kernel/vt/VtManager.cpp tests/test_vt_switch.cpp kernel/SyscallDispatch.cpp
git commit -m "feat(vt): VT_SETMODE process mode + VT_RELDISP ack + release timeout"
```

### Task 5.2: nwm becomes a VT-aware graphics owner; init launches it on tty7

**Files:**
- Modify: `user/nwm/nwm.c`, `user/init.c`

- [ ] **Step 1:** In nwm startup: open `/dev/tty7`, `ioctl(fd, KDSETMODE, KD_GRAPHICS)`, set up release/acquire signal handlers (`signal(SIGUSR1, on_release); signal(SIGUSR2, on_acquire);`), `ioctl(fd, VT_SETMODE, &mode)` with `mode.mode=VT_PROCESS, relsig=SIGUSR1, acqsig=SIGUSR2`. On `SIGUSR1` (release): set a flag to stop drawing + `ioctl(fd, VT_RELDISP, (void*)1)`. On `SIGUSR2` (acquire): set a flag to resume + force a full redraw. Guard the compositor's blit loop with the "owns display" flag.
```c
static volatile int g_own = 1;
static int g_vtfd;
static void on_release(int s){ (void)s; g_own = 0; ioctl(g_vtfd, VT_RELDISP, (void*)1); }
static void on_acquire(int s){ (void)s; g_own = 1; /* main loop sees g_own and full-redraws */ }
```
Only blit when `g_own`.

- [ ] **Step 2:** init launches nwm on tty7 once at boot (after the gettys): fork → `setsid`, open `/dev/tty7`, `TIOCSCTTY`, dup→0/1/2, exec `/disks/main/nanos/bin/nwm.nxe` (or wherever nwm lives). Respawn on exit like a getty.

- [ ] **Step 3: Build + QEMU** — `make image64`; boot; log in on tty1; `Ctrl+Alt+F7` → nwm desktop draws; `Ctrl+Alt+F1` → back to the text shell (text grid restored); `Ctrl+Alt+F7` → nwm redraws. Verify no framebuffer corruption on repeated switches.

- [ ] **Step 4: Commit**
```bash
git add user/nwm/nwm.c user/init.c
git commit -m "feat(vt): nwm is a VT_SETMODE graphics owner on tty7; init launches it"
```

---

## Phase 6 — QEMU smoke + verify64 + docs

### Task 6.1: `scripts/smoke-vt.sh` headless switch test

**Files:**
- Create: `scripts/smoke-vt.sh`
- Modify: `Makefile` (a `smoke-vt` target; add to `verify64`)

- [ ] **Step 1:** Write `scripts/smoke-vt.sh` modeled on `scripts/smoke-smp-stress.sh`: boot `disk/image64-grub2.img` headless with a serial log + monitor socket; wait for `nanos login:` on tty1; via the QEMU monitor `sendkey ctrl-alt-f2`; `sendkey`-type a login on tty2; `screendump /tmp/vt2.ppm`; `sendkey ctrl-alt-f1`; `screendump /tmp/vt1.ppm`; `sendkey ctrl-alt-f7`; `screendump /tmp/vt7.ppm`. Pass criteria: (a) no `Kernel panic|TRIPLE FAULT|KERNEL FAULT` in the serial log; (b) the three screendumps are pairwise different (a trivial byte-diff via `cmp`), proving the active console actually changed. (Use `sips -s format png` only if image inspection is needed; the byte-diff alone is a solid headless oracle.)

- [ ] **Step 2:** `Makefile`: add
```make
smoke-vt:
	bash scripts/smoke-vt.sh
```
and append `smoke-vt` to the `verify64` recipe + its summary echo (next to `smoke-smp-stress`/`smoke-smp-netstress`).

- [ ] **Step 3: Run** — `make image64 && make smoke-vt`. Expected: `x86_64 VT switch: PASS`.

- [ ] **Step 4: Commit**
```bash
git add scripts/smoke-vt.sh Makefile
git commit -m "test(vt): headless QEMU smoke — Ctrl+Alt+Fn switching + graphics VT"
```

### Task 6.2: Docs

**Files:**
- Modify: `docs/en/x86_64.md`, `docs/pl/x86_64.md` (new "Virtual terminals" subsection); `docs/en/filesystem.md` (`/dev/tty1..7` in the `/dev` table).

- [ ] **Step 1:** Document the VT subsystem: the `Ctrl+Alt+Fn` mapping (F1–F6 text, F7 graphics), `/dev/tty0..7`/`tty`/`console`, the kernel `VtManager`/`VtConsole`, and the `VT_SETMODE` graphics handoff. Update the `/dev` table in `filesystem.md` to list `tty1..tty7`.

- [ ] **Step 2: Commit**
```bash
git add docs/en/x86_64.md docs/pl/x86_64.md docs/en/filesystem.md
git commit -m "docs(vt): document virtual terminals + /dev/ttyN"
```

### Task 6.3: Full gate

- [ ] **Step 1:** `make verify64`. Expected: host tests (incl. the new VT tests) green; `check-arch` clean; BIOS/UEFI/big-RAM/e1000e/USB/SMP smokes still pass; `smoke-vt` PASS.

- [ ] **Step 2:** Update memory: add a `nanos-virtual-terminals.md` memory + `MEMORY.md` index line summarizing the VT subsystem (kernel VtManager/VtConsole, tty1-6 text + tty7 graphics, Ctrl+Alt+Fn, VT_SETMODE handoff, PTY untouched).

- [ ] **Step 3:** Finish via **superpowers:finishing-a-development-branch** (verify tests → present merge options).

---

## Self-review notes (spec coverage)

- §1 core → Phase 0–1 (FbConsole live/repaint, VtConsole, VtManager). §2 input+combo → Phase 2. §3 devices+fds+job control → Phase 3. §4 graphics handoff → Phase 5. §5 init+printk/panic → Phase 4 (+ kernel console in 2.3). §6 testing → host tests throughout + Phase 6 smoke.
- Risk (graphics release in IRQ): handled by `requestSwitch` deferring graphics releases to `servicePending()` in thread context (Phase 2.2 / 5.1).
- Risk (isConsole rework): Phase 3.3 keeps the proven fast-path but binds it to a VT, and steers init's children onto real `/dev/ttyN` device fds; existing syscall/job-control tests guard regressions.
- Symbol consistency: `kVtCount`/`kVtGraphics` (VtManager.h), `g_vtmgr`, `VtConsole`/`VtManager`/`VtTty`, `switchTo`/`requestSwitch`/`servicePending`/`relDisp`/`releaseTimeoutTick`, `setLive`/`repaintAll`/`live` used consistently across tasks.
- **Verify before coding:** the exact `Scheduler` sleep API (`sleepOn` vs `sleepOnUntil` signature), the per-pid signal sender name (`signalSendPid`/`signalSend`), `TIOCSCTTY`/`IOCTL_*` constant names, and that `Scheduler::onTick` runs in thread context — match the live code when implementing (noted inline at each use).
