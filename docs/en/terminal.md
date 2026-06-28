# NanOS Console, TTY & Terminal

The text path from the kernel boot log to an interactive shell: a machine-independent **console**
over a swappable sink (VGA text → framebuffer glyphs), an **evdev keyboard** (`/dev/input0`), a
**PTY** (`/dev/ptmx` + `/dev/pts0` + `/dev/tty`) with a line discipline + job-control signals, and
a shared **VT/ANSI engine** used by both the kernel framebuffer console and the userland terminals
(`nterm`, `terminal`). For the GUI compositor see windowing.md; for `/dev/fb0` + `/dev/input*`
in the namespace see filesystem.md.

---

## 1. Kernel console (`drivers/Console.*` + `<arch/console.h>`)

`Console` is the MI formatting layer (`write`/`writeLine`/`writeHex`, decimal/hex itoa, cursor,
scroll) that drives a character-cell **sink** through the arch contract: `consolePutChar`,
`consoleClear`, `consoleSetCursor`, `consoleInit`, `consoleActivateFramebuffer`, `consoleSize`
(the last reports the grid for `TIOCGWINSZ`). All kernel boot output goes through here.

---

## 2. The sink switch: VGA text → framebuffer console

The x86 sink (`arch/x86/drivers/console_x86.cpp`) starts in **VGA text mode**: an 80×25 cell grid at
`0xB8000` (char + attribute byte), hardware cursor via ports `0x3D4/0x3D5`, scroll by `memcpy`-ing
24 rows up. `consolePutChar` interprets `\b \t \r \n`.

When the boot framebuffer is mapped (boot.md §4), `consoleActivateFramebuffer()` flips
`g_useFb` so every sink call routes to the **framebuffer console** (`drivers/FbConsole.*`) instead —
the boot `[ OK ]` splash from that point renders as pixel glyphs. `FbConsole` is built on the shared
**VT engine** (§6) as its grid + escape parser, keeps a shadow grid to repaint only changed cells,
and draws 8×16 glyphs with an underline cursor. `consoleSize` then returns the fbcon grid
dimensions instead of 80×25.

---

## 3. Framebuffer (`drivers/Framebuffer.*`, `Fbdev.*`, `Fb0Device.*`)

`FbSurface { base, pitch, width, height, bpp }` + pure rasterizers (`fbPutPixel`, `fbFillRect`,
`fbBlitGlyph`, `fbScrollUp`) — host-testable software rendering, BGRX (32bpp) or BGR (24bpp). Two
consumers:

- **kernel fbcon** (`FbConsole`) draws glyphs straight into the mapped framebuffer for the boot log;
- **`/dev/fb0`** (`Fb0Device` over `Fbdev`) is the Linux fbdev char device: `FBIOGET_VSCREENINFO`/
  `FSCREENINFO` ioctls, `read`/`write`, and `mmap` of the linear framebuffer. `nwm` and `nterm`
  open it, `mmap` it, and take over the screen (windowing.md §6).

The boot framebuffer (addr/pitch/w/h/bpp) is discovered from the Multiboot info (`bootFramebuffer`),
requested via the Multiboot VIDEO flag (boot.md §1). Without one, the console stays VGA text.

---

## 4. Keyboard / evdev (`/dev/input0`)

PS/2 IRQ1 is owned by the **kbd kext** (kext.md), which feeds scancodes to the kernel via
`arch::inputFeedScancode` → it goes **two ways at once**:

1. **`/dev/input0`** (`drivers/KeyboardDevice.*`): a Linux-style evdev — a ring of 2-byte
   `[code][down]` events (bit 7 = extended/`0xE0`), read whole, non-blocking (`-EAGAIN` when empty).
   This is what GUI clients and `nterm` read.
2. **the cooked console** (`KeyDecoder` → line discipline): scancode→ASCII (set-1, US layout,
   Ctrl/Shift), arrows → ANSI sequences, and the control keys raise signals — `Ctrl+C`→SIGINT,
   `Ctrl+\`→SIGQUIT, `Ctrl+Z`→SIGTSTP via `consoleSignal`. `termmode`(501) switches this between
   cooked (canonical + echo + signals) and raw (bytes straight through). Blocked readers wake via a
   wait queue (scheduler.md §4).

---

## 5. PTY (`drivers/Pty.*`) — `/dev/ptmx`, `/dev/pts0`, `/dev/tty`

A pseudo-terminal pair: the **master** (`/dev/ptmx`, held by the terminal emulator) and the
**slave** (`/dev/pts0`, the shell's controlling tty; also aliased as `/dev/tty`). Two byte rings —
`m2s` (input) and `s2m` (output) — plus a **line discipline** on the input side:

- termios defaults: canonical (`ICANON`), echo, signals (`ISIG`), `ICRNL` in / `ONLCR` out.
- control chars: `VINTR`=Ctrl+C, `VQUIT`=Ctrl+\, `VSUSP`=Ctrl+Z, `VERASE`=DEL, `VEOF`=Ctrl+D.
- writing the master applies the discipline per byte: a `VINTR`/etc. calls the signal fn
  (`ptySignal` → `consoleSignalGroup`) to deliver to the **foreground process group**; canonical
  mode buffers a line (handling erase/EOF), echoes it, and flushes to the slave on newline; the
  slave's output gets `ONLCR` post-processing.
- ioctls: `TCGETS/TCSETS{,W,F}` (termios), `TIOCGWINSZ/TIOCSWINSZ` (size), `TIOCGPGRP/TIOCSPGRP`
  (foreground pgrp — job control), `TIOCPKT` (packet mode, for telnetd). The master refuses
  `TIOC*PGRP` so the emulator isn't itself a controlling terminal.

**Job control:** the shell `tcsetpgrp`s the foreground group; `Ctrl+C` on the master then routes
SIGINT to exactly that group (scheduler.md §5). The userland side is `termios.c` (tcgetattr/
tcsetattr/cfmakeraw/tcsetpgrp) and `ptyutil.c` (`openpty`/`forkpty`/`login_tty` for telnetd).

---

## 6. The VT/ANSI engine (`user/term/vt.*`)

A **pure** VT100/xterm state machine — no I/O, no globals, host-tested — **shared** by the kernel
fbcon (`FbConsole`) and the userland terminals (`nterm`, `terminal`). It holds the cell grid
(`ch, fg, bg`), cursor, SGR attributes (16 base + 256-color + truecolor palette), scroll region,
saved cursor, an **alt screen** (DECSET 47/1047/1049), and a per-row **dirty** bitmap so a renderer
repaints only changed rows. `vt_feed(bytes)` runs the parser (NORMAL/ESC/CSI/OSC): printable →
glyph; CSI finals → cursor moves, erase (`J`/`K`), `SGR` (`m`), scroll region (`r`), save/restore,
alt-screen swap; OSC swallowed to BEL/ST. The font is `vtfont.c` (8×16, shared with libnw's
`nw_gfx`).

---

## 7. The terminals: `nterm` vs `terminal`

- **`nterm`** (`user/term/nterm.c`) — a **fullscreen** terminal: it `mmap`s `/dev/fb0` directly,
  sizes a `vt` grid to the framebuffer, opens `/dev/ptmx`, `fork`s a shell on `/dev/pts0`
  (`TERM=xterm-256color`), and its `poll` loop pumps master→`vt_feed`→render-dirty and
  keyboard(`/dev/input0`)→master. It is the terminal when there's no compositor.
- **`terminal`** (`user/terminal/`) — the **windowed** terminal: same `vt` engine + pty + shell, but a
  NanWM client drawing into a window via `libnw` and polling the compositor's event fd alongside the
  pty master (windowing.md §7).

Both reuse the exact same VT engine and font; only the pixel destination and input source differ.

---

## 8. Boot wiring

`Kernel::start` registers the device nodes (boot.md §4): `/dev/fb0` (`Fb0Device`, if a
framebuffer), `/dev/input0` (`KeyboardDevice` + `kbdRegister` so the IRQ path feeds it),
`/dev/ptmx` + `/dev/pts0` + `/dev/tty` (one `Pty`, with `ptySignal` wired for Ctrl+C → pgrp). The
kernel console (VGA→fbcon) carries the boot log; once `init` starts the shell, interaction goes
through the PTY (and `nterm`/`terminal` for a full terminal).

**Key files:** `drivers/{Console,FbConsole,Framebuffer,Fbdev,Fb0Device,KeyboardDevice,Pty}.*`,
`arch/x86/drivers/{console_x86,input_x86}.cpp`, `kernel/KeyDecoder.*`,
`user/term/{vt.*, vtfont.c, nterm.c}`, `user/libc-glue/{termios.c, ptyutil.c}`.
