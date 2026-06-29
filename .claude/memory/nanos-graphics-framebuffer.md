---
name: nanos-graphics-framebuffer
description: "NanOS graphics — firmware framebuffer (vesafb/fbcon) + Linux /dev/fb0, all 4 stages DONE"
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

NanOS has graphics, done the way Linux does it with GRUB (firmware sets the mode, the
kernel draws on the linear framebuffer — no GPU driver). Built in 4 committed stages on
branch `dockerized-build` (not pushed). See [[nanos-multiprocessing-roadmap]] and
`docs/superpowers/ROADMAP.md` (Stage 6).

- **A — firmware framebuffer:** `arch/x86/boot/loader.s` Multiboot VIDEO flag + 1024×768×32
  request; `MultibootInfo` extended with fb fields (+ `multibootFramebuffer()` pure parser,
  host-tested offsets); `arch::bootFramebuffer()`. `arch::mmuMapKernelMmio` identity-maps the
  LFB (it sits ABOVE RAM, outside the kernel identity map) into the kernel dir before any
  process space is created.
- **B — renderer (MI, `drivers/Framebuffer.*`):** FbSurface + fbPutPixel/fbFillRect/
  fbBlitGlyph/fbScrollUp. ALWAYS stride by pitch; 32bpp (BGRX) + 24bpp. fbFillRect writes
  rows directly + fbScrollUp copies 32-bit words (per-pixel was too slow under TCG). Font =
  public-domain IBM VGA 8×16 (`drivers/Font8x16.*`, the real crisp VGA font from
  spacerace/romfont IBM_VGA_8x16.bin = same glyphs as Linux CONFIG_FONT_8x16; NOT the
  doubled BIOS_D variant, which looked chunky).
- **C — fbcon (`drivers/FbConsole.*`):** text grid over the framebuffer; `console_x86` picks
  fbcon vs VGA text at boot via `arch::consoleActivateFramebuffer()` (called after the LFB is
  mapped). Boot text + nsh render as glyphs at 1024×768. **Has minimal ANSI SGR color**
  (`\033[...m`: reset/bold/30-37/90-97, 16-color palette) — first step toward TUI/ncurses.
- **Boot splash:** Kernel prints Linux-style green `[ OK ]` lines per subsystem (via SGR);
  the init task waits ~2s (Scheduler::ticks) then clears the screen before launching nsh.
- **D — Linux `/dev/fb0`:** `drivers/Fbdev.*` (real `fb_fix/var_screeninfo` + FBIOGET_*),
  `CharDevice`/`Fb0Device`, `SynthFs SK_CHARDEV` (`addChar`), `Vfs`/`Syscalls` write/ioctl/
  mmap (`SYS_ioctl 54`, `SYS_mmap2 192`; mmap uses a simple 3-arg fd/len/off ABI, libc wrapper
  repacks POSIX args). `arch::mmuMapUserFb` maps the FB into the process at `0x10000000`.
  `user/fbtest.c` = gradient demo.

Gotchas burned in: (1) global ctors are NOT run, so `FbConsole::init` sets fg/bg itself
(else black-on-black). (2) `mmuMapUserFb` must switch to the kernel CR3 before allocating
page-table frames — the process dir's user-window PDE does not identity-map all RAM (caused
a #PF at a low phys addr otherwise). (3) per-pixel fbFillRect is too slow under QEMU TCG.
(4) Multiboot graphics header fields are positional (5 zero address dwords before them).

Realism note (told the user): firmware framebuffer works on real PC + any GRUB-booted
bootloader (GRUB uses VBE on BIOS / GOP on UEFI). Native 1080p on modern PCs = UEFI+GOP,
which would be a separate boot rewrite (NanOS is a Multiboot1 BIOS kernel today).
