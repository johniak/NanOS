# 02 - Ozone Platform For NanOS

**Goal:** Implement Chromium's display/input backend as an explicit NanOS Ozone platform.

**Rule:** Do not pretend NanOS is X11, Wayland, or GTK. Add a real NanOS backend so future Electron
apps use the same path.

**Entry criteria:** subplan 01 gate green (`base` builds for NanOS).

## Decisions

- **Start by cloning the headless platform.** `ui/ozone/platform/headless/` is upstream's
  smallest complete Ozone backend (software rendering into buffers, no display server). Copy it
  to `ui/ozone/platform/nanos/`, rename classes, get it BUILDING first — then replace its
  present path with NanWM and its null input with NanOS input. This gives a compilable skeleton
  on day one instead of a blank page.
- **Presentation path: NanWM client window through `libnw`** (the same client library every NanOS
  GUI app uses — see `docs/en/windowing.md`, `user/libnw/`). Direct `/dev/fb0` is allowed only
  as a throwaway debug aid, never a gate path.
- **Software compositing only in this plan.** One top-level window, no multi-window, no GPU. The
  accelerated path through the existing DRM/EGL stack is a documented follow-up after content
  shell works.
- **Clipboard: stub now** (empty read, ignored write, ladder-3 comment). Real NanWM clipboard
  mapping happens in the Electron plan (04) where `clipboard` API lands.

## Files

Chromium-side (all new code under one directory):

- `ui/ozone/platform/nanos/` — the backend (see skeleton below)
- `ui/ozone/BUILD.gn`, `ui/ozone/platform_list.txt`-family — register platform `nanos`
  (mirror how `headless` is registered; the `ozone_platform_nanos` GN arg from the canonical
  args file keys this)

NanOS-side references (read, don't modify unless a real bug):

- `user/libnw/` (window create/present/event API), `user/libnwui/`
- `docs/en/windowing.md`, `docs/en/graphics.md`

## Skeleton (the classes you will end up with)

Renamed clone of headless, then modified. Names are the contract for later subplans:

```text
ui/ozone/platform/nanos/
  BUILD.gn
  ozone_platform_nanos.{h,cc}     # class OzonePlatformNanos : public OzonePlatform
                                  #   CreatePlatformWindow() -> NanosWindow
                                  #   CreateSurfaceFactoryOzone() -> NanosSurfaceFactory
                                  #   CreatePlatformScreen(), GetPlatformEventSource()
  nanos_window.{h,cc}             # class NanosWindow : public PlatformWindow
                                  #   wraps one libnw window; Show/Hide/SetBounds/Close
                                  #   forwards damage from SchedulePaint via the surface
  nanos_surface_factory.{h,cc}    # class NanosSurfaceFactory : public SurfaceFactoryOzone
                                  #   CreateCanvasForWidget() -> software SkSurface whose
                                  #   Present() blits pixels into the libnw window buffer
  nanos_event_source.{h,cc}       # class NanosEventSource : public PlatformEventSource
                                  #   pumps libnw events -> ui::KeyEvent / ui::MouseEvent
                                  #   (keycode translation table lives here)
  nanos_screen.{h,cc}             # class NanosScreen : public PlatformScreen
                                  #   one display, size from NanWM/fb info
```

Method-level details come from the interfaces in `ui/ozone/public/*.h` at the pinned revision —
override exactly what headless overrides, plus real present + real input.

## Steps

- [ ] **Step 1: Clone + rename.** Copy `ui/ozone/platform/headless` → `ui/ozone/platform/nanos`,
  rename files/classes per the skeleton, register the platform in `ui/ozone` GN/generator lists
  (grep for `headless` there and mirror every hit). Gate for this step:

```sh
ninja -C "$SDK_WORK/chromium-src/src/out/NanOS" ui/ozone && echo OZONE-BUILDS
```

- [ ] **Step 2: Present path.** Replace the headless "write PNG/nothing" present with libnw:
  window create on `NanosWindow` construction, `Present()` copies the SkSurface pixels into the
  libnw window surface and commits. Match pixel format explicitly (record NanWM's format —
  ARGB/XRGB — in a comment; a swapped-channel window is the classic first bug).
- [ ] **Step 3: Input path.** `NanosEventSource` pumps the libnw event queue on the UI thread
  (integrate with the epoll message pump — libnw's event fd goes into the pump, no busy poll).
  Translate: key press/release with NanOS keycodes → `ui::KeyEvent` (build the table for
  printable ASCII + arrows + enter/backspace/tab/esc first), pointer move/button →
  `ui::MouseEvent`, window close → delegate `OnCloseRequest()`.
- [ ] **Step 4: Standalone smoke via upstream `ozone_demo`** (upstream target exactly for this):

```sh
ninja -C "$SDK_WORK/chromium-src/src/out/NanOS" ozone_demo
```

  Convert to `.nxe`, stage into the image, run on NanOS:

```sh
ozone_demo.nxe --ozone-platform=nanos
```

  Expected: a NanWM window animating the demo's color fill. If `ozone_demo` drags in too much
  (GL), write a 100-line `nanos_ozone_smoke` target in `ui/ozone/platform/nanos/test/` that
  creates a window, fills a known pattern (red/green/blue thirds), and exits on any key — same
  gate value.
- [ ] **Step 5: Input proof.** Extend the smoke: pattern changes color on key press, window
  closes on `q`, logs pointer coordinates on click (serial log assertable).

## Gate

- [ ] Ozone/NanOS target builds: `OZONE-BUILDS` prints.
- [ ] The window/pattern smoke renders on NanOS (QEMU screenshot or framebuffer hash — reuse the
  oracle style from `scripts/gl-desktop-test.sh`).
- [ ] Input proven: serial log shows one key event and one pointer event handled.
- [ ] No MarkText-specific assumptions in the backend.
- [ ] `status.md` row added; parent coordinator checkbox 02 ticked.
