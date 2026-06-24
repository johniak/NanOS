# Virtual Terminals (Ctrl+Alt+Fn) — Design

**Date:** 2026-06-24
**Goal:** Linux-style virtual consoles for NanOS — N kernel text consoles (F1–F6) plus a
graphics console (F7), switched with `Ctrl+Alt+Fn`, with the VT machinery living in the
kernel (faithful to Linux), reusing the existing fbcon, PS/2 input path, sessions and job
control.

**Decisions (locked during brainstorming):**
1. VT logic lives in the **kernel** (not a userspace multiplexer). The kernel sees scancodes
   first, owns the framebuffer, and already routes the console.
2. Text consoles F1–F6 are **kernel fbcon** consoles (extended `FbConsole`), with `login`/shell
   running directly on `/dev/ttyN` — the tty *is* the console, no PTY.
3. The **graphics VT (F7) ships in the first version** — full `/dev/fb0` ownership handoff with a
   Linux-style `VT_SETMODE`/`VT_RELDISP` acquire/release protocol, and `nwm` wired as F7.
4. Layout: **6 text consoles (tty1–6) + 1 graphics (tty7)**, all spawned at boot by init;
   active-after-boot = tty1.

---

## Current state (what we build on)

- **The "console" is a kernel singleton.** `arch/x86_64/drivers/input_x86_64.cpp` holds ONE
  `LineDiscipline g_line`, one raw ring (`g_rawbuf`), one `WaitQueue g_inputWq`, one `g_raw`
  flag. `arch::inputFeedScancode()` (called from the PS/2 kext via `knx_feed_scancode` →
  `KernelExports`) feeds it; `arch::inputRead()` blocks on it.
- **Output** singleton: `arch/x86_64/drivers/console_x86_64.cpp` owns one `FbConsole g_fb`
  (`g_useFb` after `consoleActivateFramebuffer`); `arch::consolePutChar` rasterizes the 8×16
  font via the shared `vt.c` engine. Early boot = VGA text at 0xB8000.
- **Console fds.** `kernel/Syscall.{h,cpp}`: fds 0/1/2 carry `isConsole=true`; `read` →
  `arch::inputRead`, `write` → `consoleWrite` (`Console::write`), `TCGETS/TCSETS` → one
  `consoleTermios`, raw mode via `consoleRaw()`.
- **Job control.** `kernel/Exec.cpp` has one global `g_consolePgrp`; `consoleSignal(sig)`
  (Ctrl+C/\\/Z from the input layer) → `signalSendGroup(g_consolePgrp, sig)`.
  `consoleSetPgrp`/`consoleGetPgrp` are the `TIOCSPGRP`/`TIOCGPGRP` backing.
- **PTY is orthogonal.** `/dev/ptmx`+`/dev/pts0`+`/dev/tty` (one `Pty`, `drivers/Pty.*`) serve
  the userspace terminal emulator `nterm` and remote logins (telnet/ssh). **Not touched** by
  this work — VT multiplexes only the *physical* console (keyboard + framebuffer).
- **init** (`user/init.c`, PID 1) runs DHCP + services, then loops: `tcsetpgrp(0,getpgrp())`,
  fork a `login` on the (single) console fds 0/1/2, reap, respawn.
- `FbConsole` (`drivers/FbConsole.h`) = `vt` engine + `FbSurface` + `m_shadow` (only changed
  cells repainted). `KeyDecoder` tracks Ctrl/Shift but **not Alt**.

---

## Section 1 — Core (`VtConsole` + `VtManager`)

New MI subsystem under `kernel/vt/` (host-testable; FS/console logic is machine-independent).

### `VtConsole` — one virtual console
Bundles, per-VT, what is global today:
- **Input state:** a `LineDiscipline`, a raw-byte ring, a `WaitQueue`, a `Termios`, a raw flag.
  (Moved out of the `input_x86_64.cpp` singleton; the arch layer keeps only the scancode→event
  decode and calls into the *active* `VtConsole`.)
- **fbcon:** its own `FbConsole` (vt grid + shadow) plus a **`live`** bit. When not live,
  `putChar` feeds the vt engine (updates the grid) but **skips the pixel blit**; dirty flags
  accumulate. `repaintAll()` blits the whole grid to the LFB and re-syncs the shadow.
- **Foreground pgrp** (replaces the global `g_consolePgrp`) + `signalFg(sig)`.
- **Mode:** `KD_TEXT` (kernel draws fbcon) or `KD_GRAPHICS` (kernel does not draw; a userspace
  owner draws via `/dev/fb0`). Plus `VT_SETMODE` process-mode state: owner pid, release/acquire
  signal numbers, and a release-ack flag.

Key methods: `feedEvent(ev)` (run line discipline / raw ring / control-key signals / wake),
`read(buf,n,nonblock)` (blocking via its WaitQueue), `write(buf,n)`, `setLive(bool)`,
`repaintAll()`, termios get/set, fg-pgrp get/set.

### `VtManager` — owns the consoles + the real framebuffer
- Holds `vt[1..7]`, the **active index**, and the real `FbSurface` (LFB from
  `bootFramebuffer()`).
- `switchTo(n)`:
  1. **Release current:** text → `setLive(false)`. Graphics in process-mode → send its release
     signal to the owner and wait for `VT_RELDISP` ack (bounded timeout; on timeout, take over
     anyway — a late frame is corrected by the next repaint).
  2. Set active = `n`.
  3. **Acquire new:** text → `setLive(true)` + `repaintAll()`. Graphics → send its acquire
     signal (owner redraws; nwm fully redraws each frame).
- `activeVt()`, `vtAt(n)`. `switchTo` is callable from IRQ context (the keyboard path) — it only
  flips flags + repaints (no blocking alloc); the graphics release-wait runs in thread context
  via a deferred path (the switch is requested, the ack-wait happens off the IRQ — see §4).

### Rendering model
Faithful to Linux fbcon: each VT keeps only the **character grid** (~0.5 MB × 7 ≈ 3.5 MB at
512 MB RAM — fine), no per-VT pixel buffers. Only the active VT blits; a switch is a full grid
repaint. The shadow tracks what is currently on the LFB and is reset on switch to force a clean
repaint.

---

## Section 2 — Input + `Ctrl+Alt+Fn` intercept

- A tiny **VT key matcher** at the very front of `arch::inputFeedScancode` tracks Ctrl
  (0x1D / 0xE0 0x1D), Alt (0x38 / 0xE0 0x38 right-alt), and F1..F7 make codes (0x3B..0x41). On
  `Ctrl+Alt+Fn` it calls `VtManager::switchTo(n)` and **consumes** the scancode (not delivered to
  any consumer). Optional later: `Alt+Left/Right` to cycle. This matcher is the only arch-side
  addition; it sits before the existing decode.
- Otherwise the scancode is routed to the **active** `VtConsole`: the per-VT `KeyDecoder` +
  line discipline / raw ring (the logic that is in the singleton today, now per-VT). Control
  keys (Ctrl+C/\\/Z) signal the **active VT's** fg pgrp.
- **`/dev/input0` (evdev)** is fed only for the active VT, so a background graphics app (nwm on
  F7) stops receiving keys when you switch away. (Today `kbdFeed` is unconditional; gate it on
  "active VT is the consumer".)
- `arch::inputRead`/`inputSetRaw`/`inputReady` become operations on a **specific** `VtConsole`
  (the caller's controlling VT), not the global singleton.

---

## Section 3 — Device nodes + job control

- **`/dev/tty1`..`/dev/tty7`** — a `VtTty` `CharDevice` wrapping a `VtConsole`. `read`/`write`
  delegate to the VT (write rasterizes iff that VT is live). ioctls:
  `TIOCGPGRP`/`TIOCSPGRP` (per-VT fg pgrp), `TCGETS`/`TCSETS` (per-VT termios), `TIOCSCTTY`
  (claim as controlling tty), `TIOCGWINSZ`, and the VT control set:
  `VT_OPENQRY`, `VT_GETSTATE`, `VT_ACTIVATE`, `VT_WAITACTIVE`, `VT_SETMODE`, `VT_RELDISP`,
  `KDSETMODE`/`KDGETMODE` (`KD_TEXT`/`KD_GRAPHICS`).
- **`/dev/tty0`** = the active VT; **`/dev/tty`** = the caller's controlling VT; **`/dev/console`**
  = tty1 (kernel console). Registered in `Kernel.cpp` next to the existing `/dev` nodes.
- **Console fds rework** (`kernel/Syscall.{h,cpp}`): an `isConsole` fd gains a **VT binding**
  (`VtConsole*` / index). `read`/`write`/termios/poll route to that VT instead of the global
  arch singleton + global `consoleTermios`. Default (fds 0/1/2 created without an explicit open)
  bind to **tty1**; `open("/dev/ttyN")` + `dup2` rebinds (how init sets up each console child).
- **Per-VT job control:** `g_consolePgrp` moves into `VtConsole`. `consoleSignal(sig)` targets
  the **active VT's** fg pgrp; `consoleSetPgrp`/`GetPgrp` act on the caller's VT. The per-process
  **controlling-tty** = a VT index, set by `TIOCSCTTY`, read by `/dev/tty`.

---

## Section 4 — Graphics VT (F7) + nwm

- nwm opens `/dev/tty7`, sets `KDSETMODE(KD_GRAPHICS)` (kernel stops drawing fbcon on that VT),
  then `VT_SETMODE(VT_PROCESS, relsig, acqsig)`, `mmap`s `/dev/fb0`, and draws.
- **Switch away from F7:** `VtManager` sends `relsig` to the owner. The kernel does NOT
  immediately overwrite the LFB; it waits (bounded) for the owner's `VT_RELDISP` ack, then
  activates the text VT and repaints. The owner, on `relsig`, stops drawing and acks. The IRQ
  only *requests* the switch (sets a pending target + wakes a VT worker / runs on the next
  thread-context tick); the ack-wait + repaint happen in thread context, never in the IRQ.
- **Switch to F7:** `VtManager` activates tty7 (KD_GRAPHICS → kernel draws nothing) and sends
  `acqsig`; the owner redraws the whole frame. No kernel-side framebuffer save is needed (the
  owner redraws), matching Linux's cooperative VT discipline.
- Coherence is cooperative (as in Linux): a misbehaving owner that draws after release corrupts
  at most one frame, corrected by the text repaint. No hard `/dev/fb0` write-gate is invented.

---

## Section 5 — init + kernel printk/panic

- **init.c:** for tty1..tty6, fork a child that `setsid()`s, `open("/dev/ttyN")`, `TIOCSCTTY`,
  `dup2`→0/1/2, then exec `login` (same login path as today). Launch `nwm` on tty7. Keep the
  reap loop, respawning each console's `login` when it exits (getty-style, per VT). `tcsetpgrp`
  is now per the child's own ttyN.
- **Kernel printk / boot text** targets **tty1**'s grid (visible when tty1 is active — boot
  messages then the tty1 login, like Linux). Early boot (before `VtManager` exists) stays on the
  bare `g_fb`/VGA path as today; once VTs are up, `consolePutChar` routes to tty1.
- **Panic:** force-switch to tty1 (`KD_TEXT`) and print, so a panic is visible even if F7
  (graphics) was active.

---

## Section 6 — Testing

- **Host tests (MI, no QEMU):** `VtManager`/`VtConsole` switching logic against a fake
  `FbSurface` + a fake signal sink:
  - active index + `live` bits flip correctly on `switchTo`;
  - `repaintAll` is invoked on the newly-active text VT;
  - text→text switch is silent (no signals); text→graphics sends acquire; graphics→text sends
    release then waits for ack then repaints;
  - `VT_RELDISP` ack releases the wait; ack timeout takes over anyway;
  - control keys / `consoleSignal` route to the **active** VT's fg pgrp;
  - per-VT termios + raw flag are independent.
  Added to `TEST_MODULES`/`COV_PATTERNS`; ≥90% line coverage on the new modules.
- **`make check-arch` stays clean:** the VT subsystem is MI; only the scancode matcher lives in
  the (already MD) arch input layer.
- **QEMU headless smoke** (new `scripts/smoke-vt.sh`, wired into `verify64`): boot, confirm a
  login prompt on tty1; via the QEMU monitor `sendkey ctrl-alt-f2`, `screendump`, confirm tty2
  is a distinct console; switch to F7, confirm nwm renders; switch back to a text VT and confirm
  the text grid is restored. No regressions in existing smokes (boot/ping/usb/smp).

---

## Files (new / changed)

**New:**
- `kernel/vt/VtConsole.{h,cpp}`, `kernel/vt/VtManager.{h,cpp}` — the subsystem.
- `drivers/VtTty.{h,cpp}` — the `/dev/ttyN` CharDevice over a `VtConsole`.
- `kernel/vt/VtIoctl.h` — `VT_*` / `KD*` constants + `vt_mode`/`vt_stat` structs (Linux ABI).
- Tests: `tests/test_vt_switch.cpp`, `tests/test_vt_console.cpp`.
- `scripts/smoke-vt.sh`.

**Changed:**
- `arch/x86_64/drivers/input_x86_64.cpp` (+ `arch/x86/drivers/input_x86.cpp` for parity): move
  the input singleton into `VtConsole`; add the `Ctrl+Alt+Fn` matcher; route to the active VT;
  gate `/dev/input0`.
- `arch/x86_64/drivers/console_x86_64.cpp`: `consolePutChar` routes to tty1's `FbConsole`;
  `VtManager` owns the LFB `FbSurface`.
- `drivers/FbConsole.{h,cpp}`: add the `live` bit + `repaintAll()`.
- `kernel/Syscall.{h,cpp}`: bind `isConsole` fds to a VT; route read/write/termios/poll per-VT.
- `kernel/Exec.cpp` + `kernel/SignalDispatch.h`: per-VT fg pgrp; `consoleSignal` → active VT.
- `kernel/SyscallDispatch.cpp`: the new `VT_*`/`KD*`/`TIOCSCTTY` ioctls.
- `kernel/Kernel.cpp`: construct `VtManager`, register `/dev/tty0..7`, `/dev/tty`, `/dev/console`.
- `kernel/Process.{h,cpp}`: per-process/session controlling-tty (VT index).
- `user/init.c`: spawn login on tty1..6, nwm on tty7, per-VT respawn.
- `user/nwm/nwm.c`: `KDSETMODE`/`VT_SETMODE` + release/acquire signal handlers + `VT_RELDISP`.
- `Makefile`: `TEST_MODULES`/`COV_PATTERNS` + `smoke-vt` wired into `verify64`.

## Invariants / risks

- FS/console core stays **MI** (`check-arch` clean); no x86 leaks into `kernel/vt/`.
- PTY/nterm path untouched — VT is the physical-console multiplexer only.
- Biggest risk: the **graphics release/acquire handoff** (§4) — IRQ requests the switch, but the
  ack-wait + repaint MUST run in thread context (the kernel heap/scheduler are not reentrant
  from the keyboard IRQ — same constraint that already shapes `consoleSignal`).
- Second risk: reworking the deeply-wired `isConsole` fast-path (§3) without regressing the
  proven blocking-read / poll / job-control behavior — covered by the existing syscall/job-control
  tests plus the new VT tests.
