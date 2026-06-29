---
name: nanos-gui-performance
description: "NanWM desktop drag/perf work — userland -O2, raster blend/blit, per-window frame cache; kernel -O2 blocked on an ext FS UB"
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

GUI performance pass on the NanWM compositor (plan: `docs/superpowers/plans/2026-06-11-gui-performance.md`). Goal: smooth window dragging with **zero visual change** (glass/AA/gradients pixel-identical). Done on branch `dockerized-build`, commits 80d9f9e→8674ca7.

- **Phase 0** (80d9f9e): `nwm.c` frame-time profiling behind `NWM_PROFILE` (default 0), integer µs to stderr.
- **Phase 1** (9118316): userland compiled at **-O2** (`UOPTFLAGS` in Makefile, on `USER_CFLAGS`+`DOOM_CFLAGS`) — the compositor's hot pixel loops are all userland (nwm/libnw write the mmapped fb directly), so this is the dominant win. `-fno-strict-aliasing -fno-delete-null-pointer-checks` are the safety flags.
- **Phase 2** (a99f581): `nw_blit` → row `memcpy`; `nw_draw_char`/`nw_text` resolve clip once + fast in-bounds path; shared inlined `nw_blend8` (RB-paired, `>>8` with `a += a>>7` so a=0→dst, a=255→src exact) replaces both `nw_mix` (÷255) and the compositor's `cmix`. ≤1 LSB drift in blended pixels only; opaque pixels bit-identical.
- **Phase 3** (b61fa9e): per-window **frame cache** — `struct nw_window` gained `uint32_t *frame` (window-local fw×fh, shell-allocated next to `buf`) + `int frame_dirty`. Dirty set on COMMIT / focus change / create; **a move (x/y) does NOT dirty it**. `nw_render_dirty_frames()` (shell calls before compose) re-renders dirty frames; `composite_round()` got a source-origin so it reads the window-local frame. Drag = wallpaper + glass-composite from cache + blit, no chrome/content re-render. Host test proves cached compose == live compose **bit-identically**; QEMU desktop pixel-identical (diff 0).
- **Phase 4** (frame pacing): **skipped** deliberately (plan says optional). Safe pacing needs converting `poll(-1)` to timeout-driven deferral or the last drag frame lags — regression risk > marginal gain under the poll-blocking model.

**KERNEL stays at -O0** (`KOPTFLAGS` empty in Makefile): at -O2 a latent **UB in the ext path-resolution string scan** (a basename/dirname loop, folded under the `ExtFilesystem::lchown` symbol, scans for NUL/`/` past a non-NUL-terminated `String` buffer) runs off into unmapped memory and **triple-faults during the boot-time root mount** (first fault pc in that scan, CR2 garbage like 0x840f01eb, then a PF storm draining ESP→~0). **Follow-up: fix that FS-layer UB, then enable -O2 for kernel + kext** — no kernel code is hot during compositing so GUI perf doesn't need it. See [[nanos-sdk-and-vim-port]] (the same `make`/QEMU verification flow).
