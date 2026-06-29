---
name: nanos-ui-redesign
description: "NanWM visual overhaul to the NanoOS design language (glass, blur, demo apps)"
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

The NanWM compositor + toolkit are being redesigned to a modern "NanoOS" look, from a
reference mockup at `.claude/nanoos-ui/index.html` (+ a screenshot the user supplied):
gradient wallpaper, rounded translucent "glass" windows with soft shadows + backdrop blur,
a macOS-style top bar (logo + active-app name + menu strip) and a bottom dock, and three
demo apps — **Files** (nwexp), **Settings** (nwset), **Terminal** (nwterm) — auto-opened by
nwm in a designed desktop layout.

All effects are software (no GPU): `user/libnw/nw_gfx.c` gained alpha blend, vertical
gradients, AA rounded-rect fill/stroke, separable box blur, transparent-bg text (`nw_text`),
`nw_mix`/`nw_lerp`. The compositor (`user/nwm/nw_compose.c`) renders each window into a
scratch buffer then rounded-composites it with per-window alpha over a blurred backdrop; the
wallpaper is pre-rendered once. Font stays the 8x16 VGA bitmap (no TrueType — out of scope).

**Why:** the user wants the UI genuinely beautiful, not placeholders. **How to apply:** build
+ screenshot in QEMU and compare against the mockup each iteration (`scripts/qemu-drive.py`).
Related: [[nanos-terminal-and-deferred-scheduler]].
