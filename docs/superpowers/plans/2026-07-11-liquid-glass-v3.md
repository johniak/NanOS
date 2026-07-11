# Liquid Glass v3 — Edge Refraction, Mockup Design Language, Glass Interiors, Text-on-Glass

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the NanOS GL desktop look like the approved mockup (`docs/superpowers/plans/2026-07-03-aero-liquid-glass-frames.html`): macOS-style light-bending at window edges, mockup-exact materials and shadows, light-glass app interiors, and legible text on glass.

**Architecture:** All work happens in the GL compositor's per-window fragment shader (`FS_WIN` in `user/nwm/nw_compose_gl.c`), the CPU ink pipeline (`user/libnw/nw_gfx.c`, `user/nwm/nw_compose.c`), and the widget toolkit (`user/libnwui`). The glass-client contract already exists (`NW_STYLE_GLASS_CLIENT`: client pixel top byte = ink alpha); v3 extends real alpha ink to the frame band and to app interiors, and replaces the stylized rim refraction with the physically-derived lens recipe every 2025 Liquid-Glass recreation converges on (research: `scratch/glass-research/refraction-research.md`, `scratch/glass-research/text-on-glass-research.md` in the main tree).

**Tech Stack:** C (freestanding userland), GLSL ES 1.00, stb_truetype, Rust (rsexp FFI only). Build `make image64-gl`; gate `scripts/smoke-virtio-gpu-gl.sh`.

## Global Constraints

- **Work in the worktree `/Users/johniak/Projects/NanOS-glass` (branch `feat/liquid-glass`).** Never commit to `main`/`develop` directly.
- **GLSL ES 1.00 only:** no `dFdx/dFdy`, no constant arrays, decimal float literals (`1.0` not `1.`), precision guard already in the shader. `pow` with non-const exponent allowed but prefer sqrt/mul.
- **Attach-once FBO rule (fork rule):** never create/resize/attach textures or FBOs mid-frame in `nw_compose_gl.c`. All allocation at init. (kosmickrisp ANGLE→Metal deadlocks otherwise.)
- **One magnification mechanism:** the new lens displacement REPLACES the 0.92/1.08 scale taps. Never both.
- **Visual oracle = macOS `screencapture` of the QEMU cocoa window.** The QEMU monitor `screendump` is blind on `gl=es`. Reference render: `scratch/glass-shots/design-mockup-full.png` (main tree).
- **`NWM_NO_GLASS=1` and the CPU fallback path (`NWM_NO_GL=1`) must keep working** after every task (legacy opaque frames, no black frames, no missing text).
- **Commits: no Claude/AI attribution, no Co-Authored-By/session trailers.** Plain conventional messages.
- **Mockup-exact values** (from the HTML's CSS, copy verbatim where a task cites them): window corner radius **11px**; focused slab gradient 158° `rgba(212,230,248,.30) → rgba(195,215,238,.16) 45% → rgba(205,222,242,.24)`; focused material saturation **1.65**, unfocused **1.0**, dark **1.35**; dark slab gradient 160° `rgba(10,16,26,.55) → rgba(8,13,22,.42)`; sheen 168° `rgba(255,255,255,.34) → .10 @22% → 0 @42%` (dark: `.14/.04`); outer hairline `rgba(8,16,30,.55)`; inner hairline white `.62` (top edge `.85`); focused shadows `0 26px 60px -14px rgba(4,10,24,.60)` + `0 10px 24px -10px rgba(4,10,24,.55)`, unfocused `0 14px 32px -12px rgba(4,10,24,.45)`; body ink `#17222f`, secondary ink = same at **62%** alpha; titlebar ink halo per Task 6; taskbar/menubar dark glass `rgba(12,17,28,.40–.42)`.
- **Text:** grayscale AA only (no subpixel). Icon PNG alpha is real alpha — never run it through the coverage-gamma LUT.
- Existing env knobs must keep working: `NWM_NO_GL`, `NWM_NO_GLASS`, `NWM_GL_SOLID`, `NWM_GLASS_DEBUG` (1 rim / 2 |offset| / 3 sharp grab / 4 blurred grab), `NWM_GL_TRACE`.

## Verification toolkit (used by every task)

- Build: `cd /Users/johniak/Projects/NanOS-glass && make image64-gl -j8` → must end without errors.
- GL gate: `cd /Users/johniak/Projects/NanOS-glass && bash scripts/smoke-virtio-gpu-gl.sh` → must print `PASS`. (SKIP means the fork QEMU is missing — that's a broken environment, stop and report.)
- Visual check: `bash scripts/gl-desktop-show.sh` boots the desktop in a visible QEMU window (auto-login flow is in the smoke script; for manual checks log in as `jan`/`jan` on Ctrl-Alt-F7). Capture with:
  `osascript -e 'tell application "System Events" to tell (first process whose name contains "qemu-system") to get {position, size} of front window'` then `screencapture -x -R"X,Y,W,H" shot.png`.
- CPU-fallback regression: `NWM_NO_GL=1` boot (or `NWM_NO_GLASS=1`) — windows must render classic opaque, all text visible.
- The worktree gotcha: if smokes drop to `nsh$`, rsync `bin/` from the main tree (git-ignored `bin/*.nxe` staging files).

---

### Task 1: FS_WIN edge refraction v2 — the macOS lens

Replace the stylized quadratic rim refraction with the consensus recipe: circular-arc slope profile + small-angle Snell displacement along the inward SDF gradient + chromatic aberration + caustic rim brightening + Fresnel rim. This is what produces "light bending at the edges" like macOS.

**Files:**
- Modify: `user/nwm/nw_compose_gl.c` (FS_WIN fragment shader string, ~lines 143–265; constants at 165–167)

**Interfaces:**
- Consumes: existing uniforms `u_sharp`, `u_backdrop`, `u_grab`, `u_bd_scale`, `u_rect`, `u_size_px`, `u_radius_px`, `u_focus`, `u_dark`; helper `backdrop(sampler2D t, vec2 spx)` (already clamps sample UVs into the padded grab — pitfall #1 from research is already handled).
- Produces: shader-internal values `x` (0 interior → 1 at rim), `s` (clamped circular-arc slope, max 4.0), `g` (unit outward SDF gradient) — Task 2 reuses `x`, `s`, `g` for specular/sheen. Debug mode 2 becomes `|bend| / max-bend`.

- [ ] **Step 1: Replace the rim/refraction block in FS_WIN**

In the shader string, replace this current block (between the `cin` computation and the `float luma =` vibrancy line):

```glsl
    float rim = 1.0 - clamp(-d / BEVEL, 0.0, 1.0);   // 1 at the edge, 0 on the body

    float e = 1.0;                                    // SDF gradient: points OUTWARD
    vec2 g = vec2(sd_box(pc + vec2(e, 0.0), hb, u_radius_px) - sd_box(pc - vec2(e, 0.0), hb, u_radius_px),
                  sd_box(pc + vec2(0.0, e), hb, u_radius_px) - sd_box(pc - vec2(0.0, e), hb, u_radius_px));
    g = normalize(g + vec2(1e-4));

    float refr   = REFRACT * mix(0.45, 1.0, u_focus);  // unfocused: gentler lens
    vec2  off_px = g * (rim * rim) * refr;             // outward, quadratic toward the rim

    vec3 ring;                                         // chromatic aberration on the lens
    ring.r = backdrop(u_sharp, u_rect.xy + p + off_px * 0.92).r;
    ring.g = backdrop(u_sharp, u_rect.xy + p + off_px       ).g;
    ring.b = backdrop(u_sharp, u_rect.xy + p + off_px * 1.08).b;
    vec3 body  = backdrop(u_backdrop, u_rect.xy + p);  // frosted flat body
    vec3 glass = mix(body, ring, smoothstep(0.15, 0.8, rim));
```

with the lens recipe (research §4, `scratch/glass-research/refraction-research.md`):

```glsl
    float e = 1.0;                                    // SDF gradient: points OUTWARD
    vec2 g = vec2(sd_box(pc + vec2(e, 0.0), hb, u_radius_px) - sd_box(pc - vec2(e, 0.0), hb, u_radius_px),
                  sd_box(pc + vec2(0.0, e), hb, u_radius_px) - sd_box(pc - vec2(0.0, e), hb, u_radius_px));
    g = normalize(g + vec2(1e-4));

    /* -- edge lens profile: circular arc over the bevel band ------------------
       x: 0 deep inside -> 1 exactly at the rim.
       s: circular-arc slope x/sqrt(1-x^2), clamped (floor 0.0625 => max 4.0) so
          the rim pixel doesn't explode into rainbow noise.                     */
    float x = clamp(1.0 + d / BEVEL, 0.0, 1.0);
    float s = x / sqrt(max(1.0 - x * x, 0.0625));

    /* -- displacement: small-angle Snell along -g = sample inward = magnify ---
       thickness scales with window size (bigger element = thicker glass),
       unfocused windows get a gentler lens.                                    */
    float thick = clamp(min(u_size_px.x, u_size_px.y) * 0.045, 14.0, 26.0)
                  * mix(0.55, 1.0, u_focus);
    float bend  = s * (1.0 - 1.0 / IOR) * thick;       // px, max ~ 1.33 * thick
    vec2  spx   = u_rect.xy + p - g * bend;            // screen-px sample point

    vec2 ca = g * (s * CA_PX);                         // chromatic aberration, px
    vec3 ring;
    ring.r = backdrop(u_sharp, spx - ca).r;            // "lower IOR" for red
    ring.g = backdrop(u_sharp, spx).g;
    ring.b = backdrop(u_sharp, spx + ca).b;            // "higher IOR" for blue

    /* -- lensing vs frost: rim = crisp refracted image, interior = frost ------
       (lensing concentrates light, blur scatters it — Apple's split).
       The frost also bends slightly so the transition doesn't shear.           */
    vec3 body  = backdrop(u_backdrop, u_rect.xy + p - g * bend * 0.35);
    float lens = smoothstep(0.0, 0.7, x);
    vec3 glass = mix(body, ring, lens);
    glass *= 1.0 + CAUSTIC * s * 0.25;                 // rim concentrates light: brighten
```

- [ ] **Step 2: Replace the shader constants**

Replace:

```glsl
const float BEVEL   = 14.0;      // px over which the glass edge curves
const float REFRACT = 12.0;      // max lens displacement at the rim, px (focused)
```

with:

```glsl
const float BEVEL   = 14.0;      // px over which the glass edge curves
const float IOR     = 1.50;      // glass index of refraction (small-angle Snell)
const float CA_PX   = 2.5;       // chromatic aberration offset at slope 1, px
const float CAUSTIC = 0.25;      // rim light-concentration gain
```

(`REFRACT` has no remaining users — delete it. `LIGHT` is retouched in Task 2; leave it for now.)

- [ ] **Step 3: Rewire the specular + old `rim` users to the new profile**

The existing specular block uses `rim`:

```glsl
    float facing = max(dot(g, LIGHT), 0.0);            // specular glint on the lit bevel
    glass += vec3(pow(facing, 3.0) * rim * rim * (0.35 + 0.25 * u_focus));
```

Replace with the slope-normal version plus Sorrell's Fresnel rim:

```glsl
    vec3  N      = normalize(vec3(g * s, 1.0));        // pseudo-3D surface normal
    vec3  L      = normalize(vec3(LIGHT, 0.55));       // key light, up-left, out of screen
    float spec   = pow(max(dot(N, L), 0.0), 24.0);
    float fres   = pow(x, 2.5) * 0.4;                  // fresnel-ish rim (Sorrell)
    glass += vec3((spec * (0.45 + 0.30 * u_focus) + fres * 0.5) * x);
```

Also update debug mode 2 (was `length(off_px)/refr`):

```glsl
        else if (u_debug < 2.5) col = vec3(bend / (1.33 * 26.0));
```

Debug mode 1 (`vec3(rim,0,0)`) becomes `vec3(x, 0.0, 0.0)`.

- [ ] **Step 4: Build**

Run: `cd /Users/johniak/Projects/NanOS-glass && make image64-gl -j8`
Expected: clean build. A GLSL syntax error shows up at runtime, not compile time — the smoke in Step 5 catches it (`GL backend disabled` marker → FAIL).

- [ ] **Step 5: Run the GL gate**

Run: `bash scripts/smoke-virtio-gpu-gl.sh`
Expected: `PASS: nwm GL present backend composites + scans out the desktop via GBM+EGL+KMS (glkms).` and the colour-count line ≥ 200.

- [ ] **Step 6: Visual verification of the lens**

Boot `bash scripts/gl-desktop-show.sh`, log in, and screencapture the QEMU window (toolkit commands above). Verify by eye in the capture:
- The wallpaper visibly bends/magnifies through a ~14px band at every window edge — content near the edge appears pulled outward (magnified), NOT pushed inward.
- Corners bend smoothly around the corner arc (no shearing, no radial fisheye across the window body).
- A faint cyan/warm colour fringe rides the rim (CA), and the rim band is slightly brighter than the body (caustic).
- Interior of the window body stays quiet (no fisheye).
Also capture `NWM_GLASS_DEBUG=2` (edit the harness env or use the config knob the script exposes) if anything looks off: the bend field must be a thin bright band hugging the border, black interior.

- [ ] **Step 7: Commit**

```bash
git add user/nwm/nw_compose_gl.c
git commit -m "nwm-gl: macOS-style edge lens - circular-arc slope + small-angle Snell refraction, CA, caustic rim"
```

---

### Task 2: Slab material tuning to the mockup (gradient tint, sheen, saturation, unfocused, dark)

Make the glass material itself match the mockup CSS: diagonal gradient tint instead of flat tint, Aero diagonal sheen, saturation 1.65/1.0/1.35, mockup hairlines, unfocused desaturation.

**Files:**
- Modify: `user/nwm/nw_compose_gl.c` (FS_WIN only: the vibrancy/tint/hairline section after the `glass` value from Task 1)

**Interfaces:**
- Consumes: `glass`, `x`, `s`, `g`, `v_uv`, `u_focus`, `u_dark`, `d` from Task 1.
- Produces: final `glass` colour fed to the caption-sphere loop and ink mix (unchanged downstream contract).

- [ ] **Step 1: Replace vibrancy + flat tint with saturation + gradient slab**

Replace this block:

```glsl
    float luma = dot(glass, vec3(0.299, 0.587, 0.114));   // vibrancy + frost lift
    glass = mix(vec3(luma), glass, 1.22);
    glass = glass * 0.90 + vec3(0.085);

    vec3  tcol = mix(vec3(0.76, 0.78, 0.81), vec3(0.46, 0.60, 0.80), u_focus);
    float tamt = mix(0.34, 0.22, u_focus);             // unfocused: paler, denser tint
    tcol  = mix(tcol, vec3(0.10, 0.11, 0.13), u_dark);
    tamt  = mix(tamt, 0.48, u_dark);                   // dark glass is denser
    glass = mix(glass, tcol, tamt);
```

with the mockup material (values transcribed from the CSS in the Global Constraints):

```glsl
    /* -- saturation boost: mockup saturate(1.65) focused / 1.0 unfocused / 1.35 dark */
    float luma = dot(glass, vec3(0.299, 0.587, 0.114));
    float sat  = mix(mix(1.0, 1.65, u_focus), 1.35, u_dark);
    glass = max(mix(vec3(luma), glass, sat), vec3(0.0));

    /* -- slab gradient tint: 158deg light / 160deg dark, three stops ------------ */
    float t = clamp(v_uv.x * 0.35 + v_uv.y * 0.94, 0.0, 1.0);   // diagonal coord
    /* light, focused: rgba(212,230,248,.30) -> (195,215,238,.16)@45% -> (205,222,242,.24) */
    vec3  lc = mix(vec3(0.831, 0.902, 0.973), vec3(0.765, 0.843, 0.933), smoothstep(0.0, 0.45, t));
    lc       = mix(lc, vec3(0.804, 0.871, 0.949), smoothstep(0.45, 1.0, t));
    float la = mix(0.30, 0.16, smoothstep(0.0, 0.45, t));
    la       = mix(la, 0.24, smoothstep(0.45, 1.0, t));
    /* light, unfocused: rgba(205,212,224,.20) -> (200,208,220,.10)@45% -> (202,210,222,.15) */
    vec3  uc = mix(vec3(0.804, 0.831, 0.878), vec3(0.784, 0.816, 0.863), smoothstep(0.0, 0.45, t));
    uc       = mix(uc, vec3(0.792, 0.824, 0.871), smoothstep(0.45, 1.0, t));
    float ua = mix(0.20, 0.10, smoothstep(0.0, 0.45, t));
    ua       = mix(ua, 0.15, smoothstep(0.45, 1.0, t));
    /* dark: rgba(10,16,26,.55) -> rgba(8,13,22,.42) */
    vec3  dc = mix(vec3(0.039, 0.063, 0.102), vec3(0.031, 0.051, 0.086), t);
    float da = mix(0.55, 0.42, t);
    vec3  tcol = mix(mix(uc, lc, u_focus), dc, u_dark);
    float tamt = mix(mix(ua, la, u_focus), da, u_dark);
    glass = mix(glass, tcol, tamt);

    /* -- Aero sheen: 168deg white glare, .34 -> .10@22% -> 0@42% (dark .14/.04) - */
    float ts = clamp(v_uv.y * 0.98 + v_uv.x * 0.21, 0.0, 1.0);
    float sa = mix(mix(0.34, 0.10, smoothstep(0.0, 0.22, ts)), 0.0, smoothstep(0.22, 0.42, ts));
    sa *= mix(0.5, 1.0, u_focus);                       // mockup: unfocused sheen at 50%
    sa *= mix(1.0, 0.41, u_dark);                       // dark: .14/.04 = ~0.41x
    glass = mix(glass, vec3(1.0), sa);
```

- [ ] **Step 2: Mockup hairlines**

Replace:

```glsl
    float outer = 1.0 - clamp(-d - 0.5, 0.0, 1.0);     // 1px dark outer hairline
    float inner = clamp(-d - 1.0, 0.0, 1.0) * (1.0 - clamp(-d - 2.2, 0.0, 1.0)); // 1px white inner
    glass = mix(glass, vec3(0.13, 0.16, 0.20), outer * 0.55);
    glass += vec3(inner * 0.22);
```

with (outer `rgba(8,16,30,.55)`, inner white `.62`, top edge boosted toward `.85`):

```glsl
    float outer = 1.0 - clamp(-d - 0.5, 0.0, 1.0);     // 1px dark outer hairline
    float inner = clamp(-d - 1.0, 0.0, 1.0) * (1.0 - clamp(-d - 2.2, 0.0, 1.0)); // 1px white inner
    float ostr  = mix(0.45, 0.55, u_focus);            // mockup: unfocused outer .45
    glass = mix(glass, vec3(0.031, 0.063, 0.118), outer * ostr);
    float istr  = 0.62 + 0.23 * (1.0 - smoothstep(0.0, 0.06, v_uv.y));  // top edge ~.85
    istr *= mix(1.0, 0.45, u_dark);                    // dark ring is fainter (.28)
    glass = mix(glass, vec3(1.0), inner * istr);
```

- [ ] **Step 3: Light direction constant**

The mockup's sheen and highlights come from the top-left. Keep `LIGHT` but confirm it is `vec2(-0.555, -0.832)` (top-left in y-down coords) — no change needed unless Task 1 altered it; `L` in the spec block of Task 1 already wraps it into a vec3.

- [ ] **Step 4: Build + gate**

Run: `make image64-gl -j8 && bash scripts/smoke-virtio-gpu-gl.sh`
Expected: `PASS` + colour count ≥ 200.

- [ ] **Step 5: Visual comparison against the mockup**

Screencapture the QEMU window. Open `scratch/glass-shots/design-mockup-full.png` (main tree) side by side. Check, item by item:
- Focused window slab: cool blue-white, brighter toward the top-left, visibly more colour-saturated backdrop than the unfocused one.
- Unfocused window: paler, desaturated (backdrop through it looks washed-out), fainter hairlines and sheen.
- Terminal (dark): dense dark slab, backdrop still readable through it, subtler ring.
- Diagonal white glare across the top ~40% of each slab.

- [ ] **Step 6: Commit**

```bash
git add user/nwm/nw_compose_gl.c
git commit -m "nwm-gl: mockup material - gradient slab tint, Aero sheen, saturation 1.65/1.0/1.35, mockup hairlines"
```

---

### Task 3: GL window drop shadows (SDF soft shadow pre-pass)

The GL compositor has no window shadows (the CPU path does). Add an analytic soft shadow drawn as one quad per window, before its FS_WIN quad, matching the mockup: focused `0 26px 60px -14px rgba(4,10,24,.60)` + `0 10px 24px -10px rgba(4,10,24,.55)`, unfocused `0 14px 32px -12px rgba(4,10,24,.45)`.

**Files:**
- Modify: `user/nwm/nw_compose_gl.c` (new FS_SHADOW program + one draw in the window loop of `nw_gl_frame`, ~lines 608–681)

**Interfaces:**
- Consumes: window frame rect (`wx, wy, fw, fh`), `g_radius`, focus flag — all already computed in the loop.
- Produces: nothing downstream; purely visual layer under each window.

- [ ] **Step 1: Add the FS_SHADOW shader string** (next to the other shader strings)

```c
static const char *FS_SHADOW =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "varying vec2 v_uv;\n"
    "uniform highp vec4 u_rect;\n"       /* the EXPANDED shadow quad, screen px */
    "uniform vec2  u_wsize;\n"           /* window frame w,h in px */
    "uniform float u_wradius;\n"
    "uniform float u_wfocus;\n"
    "float sd_box(vec2 p, vec2 b, float r) {\n"
    "    vec2 q = abs(p) - b + vec2(r);\n"
    "    return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;\n"
    "}\n"
    "void main() {\n"
    "    vec2  p  = v_uv * u_rect.zw;\n"                    /* quad-local px */
    "    vec2  wc = vec2(SH_PAD, SH_PAD - SH_OFFY) + 0.5 * u_wsize;\n"
    "    float d  = sd_box(p - wc, 0.5 * u_wsize, u_wradius);\n"
    "    float deep = 1.0 - smoothstep(-8.0, mix(20.0, 30.0, u_wfocus), d);\n"
    "    float a  = deep * deep * mix(0.42, 0.60, u_wfocus);\n"
    "    gl_FragColor = vec4(0.016, 0.039, 0.094, a);\n"    /* rgba(4,10,24) */
    "}\n";
```

with two `#define`-style literals folded in when writing the string: `SH_PAD` = **36.0** (px the quad extends beyond the frame on each side) and `SH_OFFY` = **10.0** (shadow drops 10px below the window — the window box inside the quad sits 10px above center). Write them as numeric literals in the GLSL (ES 1.00 has no host-side defines here): `vec2 wc = vec2(36.0, 26.0) + 0.5 * u_wsize;` (26.0 = 36.0 − 10.0).

- [ ] **Step 2: Compile/link the program at init and cache uniforms**

In `nw_gl_init`, alongside the existing programs: compile `VS_QUAD` + `FS_SHADOW` into `g_prog_shadow`; fetch `u_rect` (vertex-shared), `u_wsize`, `u_wradius`, `u_wfocus` locations into `g_sh_rect/g_sh_wsize/g_sh_wradius/g_sh_wfocus` (follow the exact pattern of the FS_WIN program setup at ~lines 402–415). No new textures, no new FBOs (fork rule untouched — this program only draws).

- [ ] **Step 3: Draw the shadow quad before each window's FS_WIN quad**

In the `nw_gl_frame` window loop, immediately BEFORE the existing FS_WIN draw for a glass window (`glass` true), insert:

```c
        if (glass) {                       /* soft SDF shadow under the slab */
            glUseProgram(g_prog_shadow);
            glUniform2f(g_sh_wsize, (float)fw, (float)fh);
            glUniform1f(g_sh_wradius, g_radius);
            glUniform1f(g_sh_wfocus, (idx == s->focus) ? 1.0f : 0.0f);
            quad(g_prog_shadow, g_sh_rect, wx - 36, wy - 26, fw + 72, fh + 72);
        }
```

(`quad()` is the existing helper that sets `u_rect`/`u_screen` and draws; match its actual signature at ~lines 364–372 — pass the shadow program's cached locations. Blending is already enabled for the window pass; the shadow quad relies on standard `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`.) The expanded quad: x−36, y−36−(−10)=y−26 top, +72 on each dimension.

- [ ] **Step 4: Build + gate**

Run: `make image64-gl -j8 && bash scripts/smoke-virtio-gpu-gl.sh`
Expected: `PASS`.

- [ ] **Step 5: Visual check**

Screencapture: every window has a soft dark shadow, densest just under the bottom edge, fading by ~30px; the focused window's shadow is visibly deeper/larger than unfocused ones; no hard square edges (the SDF falloff is rounded); no shadow on the wallpaper where there is no window.

- [ ] **Step 6: Commit**

```bash
git add user/nwm/nw_compose_gl.c
git commit -m "nwm-gl: soft SDF drop shadows under glass windows (focused deeper, mockup values)"
```

---

### Task 4: Gamma-correct glyph blending (the cov143 LUT)

`nw_blend_pixel` lerps glyph coverage directly in sRGB space — the textbook cause of thin, anemic dark text (worst on mid-luminance glass). Fix globally with the gamma-1.43 coverage remap (`a' = 255·(a/255)^(1/1.43)`), the Photoshop/Skia compromise. One LUT, applied ONLY to glyph coverage (never to icon alpha).

**Files:**
- Modify: `user/libnw/nw_gfx.c` (LUT + the three glyph paths: `nw_text` ~line 226, `nw_draw_char_t` ~line 108, and the glyph blend inside `nw_draw_char`'s AA path if it blends coverage ~line 214)
- Modify: `user/libnw/nw_gfx.h` (export the LUT accessor for Task 5/6)

**Interfaces:**
- Produces: `const uint8_t *nw_cov143(void)` — returns the 256-entry coverage remap table (lazily initialized, no pow() at runtime after init). Tasks 5 and 6 use it.

- [ ] **Step 1: Add the LUT to nw_gfx.c**

```c
/* Coverage -> alpha remap for glyph AA: blending sRGB with raw coverage renders dark
 * text thin and light text fat; remapping by a^(1/1.43) is the standard perceptual
 * compromise (Photoshop/Skia). Glyph coverage only — icon alpha is real alpha.       */
static uint8_t s_cov143[256];
static int     s_cov143_init;
const uint8_t *nw_cov143(void)
{
	if (!s_cov143_init) {
		for (int i = 0; i < 256; i++) {
			/* pow(i/255, 1/1.43) = a^0.699 without libm pow (freestanding):
			 * sqrt chain a^0.5 * a^0.125 * a^0.0625 = a^0.6875 — exponent error
			 * < 0.012, well under a coverage step. */
			double a = i / 255.0;
			double s1 = __builtin_sqrt(a);    /* a^0.5    */
			double s2 = __builtin_sqrt(s1);   /* a^0.25   */
			double s3 = __builtin_sqrt(s2);   /* a^0.125  */
			double s4 = __builtin_sqrt(s3);   /* a^0.0625 */
			double r  = s1 * s3 * s4;         /* a^0.6875 ~= a^(1/1.43) */
			s_cov143[i] = (uint8_t)(r * 255.0 + 0.5);
		}
		s_cov143[0] = 0; s_cov143[255] = 255;
		s_cov143_init = 1;
	}
	return s_cov143;
}
```

(If `__builtin_sqrt` is unavailable in this freestanding userland, check how `nw_fill_round` does its AA math at ~line 364 — it already computes distances; reuse whatever sqrt it uses. If there is a libm `sqrt` linked, plain `sqrt` is fine.)

- [ ] **Step 2: Apply in the glyph paths**

In `nw_text`'s inner loop (~line 242), change `nw_blend_pixel(s, X, Y, fg, a)` to `nw_blend_pixel(s, X, Y, fg, nw_cov143()[a])` — hoist `const uint8_t *lut = nw_cov143();` out of the loop. Same in `nw_draw_char_t` (~line 119) and in any other place that blends `stb` coverage (grep `nw_blend_pixel` in `nw_gfx.c` and classify each call: glyph coverage → remap; geometric AA coverage from `nw_fill_round`/`nw_stroke_round` → do NOT remap; icon alpha in `nwui_paint.c` → do NOT touch).

- [ ] **Step 3: Declare in nw_gfx.h**

```c
const uint8_t *nw_cov143(void);   /* glyph coverage -> alpha remap (gamma 1.43) */
```

- [ ] **Step 4: Build + gate + visual**

Run: `make image64-gl -j8 && bash scripts/smoke-virtio-gpu-gl.sh` → `PASS`.
Visual: screencapture; UI text everywhere (menus, titlebars, terminal) must look slightly HEAVIER/fuller than before, never thinner; no smearing or clipped glyphs. CPU fallback (`NWM_NO_GL=1`): text also fuller, nothing broken.

- [ ] **Step 5: Commit**

```bash
git add user/libnw/nw_gfx.c user/libnw/nw_gfx.h
git commit -m "libnw: gamma-1.43 coverage remap for glyph AA - fixes thin dark text on glass"
```

---

### Task 5: ARGB ink primitives in libnw (straight-alpha painting)

Glass interiors and the alpha-ink titlebar need primitives that WRITE the alpha channel (today every primitive masks it out). Add straight-alpha src-over variants. Contract with the GL shader: client buffer = straight (non-premultiplied) ARGB; `ctex.a` = ink coverage; `ctex.rgb` = ink colour at full strength.

**Files:**
- Modify: `user/libnw/nw_gfx.c`, `user/libnw/nw_gfx.h`

**Interfaces:**
- Produces (exact signatures, used by Tasks 6, 7, 9):

```c
void nw_clear_argb(const struct nw_surface *s, int x, int y, int w, int h, uint32_t argb); /* raw store, incl. alpha */
void nw_over_pixel(const struct nw_surface *s, int x, int y, uint32_t argb);               /* straight src-over, writes alpha */
void nw_over_rect(const struct nw_surface *s, int x, int y, int w, int h, uint32_t argb);
void nw_over_round(const struct nw_surface *s, int x, int y, int w, int h, int r, uint32_t argb); /* AA rounded rect */
void nw_text_argb(const struct nw_surface *s, int x, int y, const char *str, uint32_t argb);      /* glyphs, cov143-remapped */
int  nw_text_argb_w(const char *str);                                                             /* == nw_text_w */
```

- [ ] **Step 1: Implement the straight src-over core**

Put the core function in a new header `user/libnw/nw_over_core.h` as `static inline` (so the host test in Step 3 can include the SAME code); `nw_gfx.c` includes it and wraps it into the public `nw_over_pixel` symbol (a one-line wrapper calling the inline). The core:

```c
/* Straight-alpha src-over that also accumulates destination alpha:
 *   outA = sa + da*(1-sa);  outC = (sc*sa + dc*da*(1-sa)) / outA
 * Dest is straight ARGB (the GL shader mixes ink as mix(glass, rgb, a)). */
void nw_over_pixel(const struct nw_surface *s, int x, int y, uint32_t argb)
{
	if (x < s->clip_x0 || y < s->clip_y0 || x > s->clip_x1 || y > s->clip_y1) return;
	uint32_t *d = s->px + (size_t)y * s->stride + x;
	unsigned sa = argb >> 24;
	if (!sa) return;
	if (sa == 255) { *d = argb; return; }
	uint32_t dst = *d;
	unsigned da  = dst >> 24;
	unsigned ra  = sa + ((da * (255 - sa) + 127) / 255);          /* out alpha */
	if (!ra) { *d = 0; return; }
	unsigned wd = da * (255 - sa) / 255;                          /* dst weight */
	unsigned sr = (argb >> 16) & 0xff, sg = (argb >> 8) & 0xff, sb = argb & 0xff;
	unsigned dr = (dst >> 16) & 0xff, dg = (dst >> 8) & 0xff, db = dst & 0xff;
	unsigned r = (sr * sa + dr * wd) / ra;
	unsigned g = (sg * sa + dg * wd) / ra;
	unsigned b = (sb * sa + db * wd) / ra;
	*d = ((uint32_t)ra << 24) | (r << 16) | (g << 8) | b;
}
```

`nw_clear_argb` = clipped raw store loop (like `nw_fill_rect` but storing the full 32-bit value). `nw_over_rect` = loop over `nw_over_pixel` (hoist clipping out of the inner loop like `nw_fill_rect` does). `nw_over_round`: copy the structure of `nw_fill_round` (~line 364) but where it computes per-pixel AA coverage `cov` (0–255), call `nw_over_pixel` with alpha `= (cov * (argb>>24)) / 255` and the same RGB.

- [ ] **Step 2: Implement `nw_text_argb`**

Copy the structure of `nw_text` (~line 226): same font, baseline, advance. Per covered pixel:

```c
	unsigned a = lut[covrow[gx]];                 /* cov143 remap (Task 4) */
	a = (a * (argb >> 24)) / 255;                 /* scale by ink alpha */
	nw_over_pixel(s, X, Y, (a << 24) | (argb & 0x00ffffff));
```

`nw_text_argb_w(str)` just calls `nw_text_w(str)`.

- [ ] **Step 3: Host-side sanity test (pure math, no OS)**

The blend core is pure C — test it on the host. Create `tests/host-argb.c`:

```c
/* cc -o /tmp/host-argb tests/host-argb.c && /tmp/host-argb  (host-only helper test) */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
struct nw_surface { uint32_t *px; int w, h, stride; int clip_x0, clip_y0, clip_x1, clip_y1; };
#define NW_ARGB_TEST_HOST 1
#include "../user/libnw/nw_over_core.h"   /* see note below */
int main(void)
{
	uint32_t buf[4] = {0, 0, 0, 0};
	struct nw_surface s = { buf, 4, 1, 4, 0, 0, 3, 0 };
	nw_over_pixel(&s, 0, 0, 0xff112233u);            assert(buf[0] == 0xff112233u); /* opaque copy */
	nw_over_pixel(&s, 1, 0, 0x80ffffffu);            /* 50% white over transparent */
	assert((buf[1] >> 24) == 0x80 && ((buf[1] >> 16) & 0xff) == 0xff);
	nw_over_pixel(&s, 1, 0, 0x80000000u | 0x000000u);/* 50% black over that */
	unsigned a = buf[1] >> 24;                        /* 0.5 + 0.5*0.5 = 0.75 */
	assert(a >= 0xbe && a <= 0xc2);
	unsigned r = (buf[1] >> 16) & 0xff;               /* (0*128 + 255*64)/191 ~ 85 */
	assert(r >= 82 && r <= 88);
	nw_over_pixel(&s, 2, 0, 0x00123456u);            assert(buf[2] == 0);          /* a=0 no-op */
	printf("host-argb OK\n");
	return 0;
}
```

To make the core includable from both the kernel-userland build and the host test, put `nw_over_pixel` (only it) in a small header `user/libnw/nw_over_core.h` as a `static inline`, included by `nw_gfx.c` (which wraps it into the public symbol) and by the test. If that split fights the existing style, alternatively duplicate the 20-line function into the test with a comment naming the source of truth — but prefer the header.

- [ ] **Step 4: Run the host test — expect it to fail first**

Write the test BEFORE the header exists: `cc -o /tmp/host-argb tests/host-argb.c` → expected compile FAILURE (missing header). Then implement Step 1's header + wrappers, re-run: `host-argb OK`.

- [ ] **Step 5: Declarations in nw_gfx.h + build**

Add the six prototypes. Run `make image64-gl -j8` → clean; `bash scripts/smoke-virtio-gpu-gl.sh` → `PASS` (no behaviour change yet — nothing calls the new primitives).

- [ ] **Step 6: Commit**

```bash
git add user/libnw/nw_gfx.c user/libnw/nw_gfx.h user/libnw/nw_over_core.h tests/host-argb.c
git commit -m "libnw: straight-alpha ARGB primitives (over_pixel/rect/round, text_argb) + host blend test"
```

---

### Task 6: Titlebar — real Aero glow, auto ink polarity, alpha-ink band

Replace the 8-copy ±1px title halo with a real Aero glow (blurred coverage sheet + core), pick glow/core polarity from the wallpaper luminance under the bar, and move the frame band from luminance keying to real alpha ink so the glow's soft fringe survives.

**Files:**
- Modify: `user/nwm/nw_compose.c` (glass branch of `draw_window_to` ~lines 112–137; new glow renderer; wallpaper ref)
- Modify: `user/nwm/nw_compose.h` (two new prototypes)
- Modify: `user/nwm/nwm_core.h` (one field in `struct nw_window`)
- Modify: `user/nwm/nwm.c` (pass the wallpaper ref once)
- Modify: `user/nwm/nw_compose_gl.c` (FS_WIN: band ink from `ctex.a` instead of the luminance key)

**Interfaces:**
- Consumes: `nw_text_argb`, `nw_over_*`, `nw_cov143` (Tasks 4–5); `nwfont_get`/`nwfont_ascent` (existing).
- Produces: `void nw_compose_set_wallpaper_ref(const struct nw_surface *wall);` and per-window `w->ink_dark` (0/1, with hysteresis). Task 9 reuses the polarity helper: `int nw_backdrop_wants_dark_ink(int x, int y, int w, int h, int prev);`

- [ ] **Step 1: Wallpaper reference + polarity helper in nw_compose.c**

```c
static const struct nw_surface *s_wall;   /* pre-rendered wallpaper (polarity sampling) */
void nw_compose_set_wallpaper_ref(const struct nw_surface *wall) { s_wall = wall; }

/* Sample a sparse 8x2 grid of wallpaper pixels under rect; gamma-space luma
 * Y=(77R+150G+29B)>>8, threshold 117 (WCAG 0.179 linear) with +/-8 hysteresis. */
int nw_backdrop_wants_dark_ink(int x, int y, int w, int h, int prev)
{
	if (!s_wall || w < 8 || h < 2) return prev >= 0 ? prev : 1;
	long acc = 0; int n = 0;
	for (int j = 0; j < 2; j++) for (int i = 0; i < 8; i++) {
		int sx = x + (w * (2 * i + 1)) / 16, sy = y + (h * (2 * j + 1)) / 4;
		if (sx < 0 || sy < 0 || sx >= s_wall->w || sy >= s_wall->h) continue;
		uint32_t p = s_wall->px[(size_t)sy * s_wall->stride + sx];
		acc += (77 * ((p >> 16) & 0xff) + 150 * ((p >> 8) & 0xff) + 29 * (p & 0xff)) >> 8;
		n++;
	}
	if (!n) return prev >= 0 ? prev : 1;
	int luma = (int)(acc / n);
	if (prev == 1 && luma < 109) return 0;      /* hysteresis band 109..125 */
	if (prev == 0 && luma > 125) return 1;
	if (prev < 0) return luma > 117;
	return prev;
}
```

Call `nw_compose_set_wallpaper_ref(...)` from `nwm.c` right after the wallpaper surface is rendered/cached (find where `nw_render_wallpaper` is invoked; pass that surface — it must outlive the session, which the shell's cache does).

- [ ] **Step 2: Per-window polarity state**

`nwm_core.h`, in `struct nw_window` next to `glass`/`style`: `int8_t ink_dark;` — initialize to `-1` where windows are created (memset-0 structs: explicitly set `w->ink_dark = -1` in the CREATE handler; find it near `w->glass = 1` at `nwm_core.c:909`). In `nw_render_dirty_frames` (`nw_compose.c:333`), before calling `draw_window_to` for a dirty window, refresh it:

```c
		w->ink_dark = (int8_t)nw_backdrop_wants_dark_ink(w->x, w->y, frame_w(w), NW_TITLEBAR_H, w->ink_dark);
```

(`nw_render_dirty_frames` has the mutable server; `draw_window_to` stays const and reads `w->ink_dark`.)

- [ ] **Step 3: The Aero glow renderer** (new static function in nw_compose.c)

```c
/* Aero caption glow: rasterize the title run's coverage once, box-blur it twice
 * (r=6 then r=3 ~ Gaussian), gain 3x -> a soft sheet; composite sheet then core
 * with real alpha (nw_over_*). dark_ink=1: light sheet + dark core (light glass);
 * dark_ink=0: dark sheet + light core (dark glass / dark wallpaper).            */
#define GLOW_R    6
#define GLOW_MAXW 512
static void draw_caption_glow(const struct nw_surface *sc, int cx, int ty,
                              const char *title, int dark_ink, int focused)
{
	static uint8_t cov[NW_TITLEBAR_H][GLOW_MAXW], tmp[NW_TITLEBAR_H][GLOW_MAXW];
	int tw = nw_text_w(title); if (tw > GLOW_MAXW - 4 * GLOW_R) tw = GLOW_MAXW - 4 * GLOW_R;
	int W = tw + 4 * GLOW_R, H = NW_TITLEBAR_H;
	memset(cov, 0, sizeof(cov));
	/* 1) coverage of the run at (2*GLOW_R, baseline) — glyph loop like nw_text,
	 *    writing max(cov, glyph) into cov[][] instead of blending to a surface.  */
	rasterize_run_coverage(&cov[0][0], W, H, 2 * GLOW_R, ty, title);
	/* 2) two-pass box blur, radius 6 then 3 (running-sum window, O(W*H)) */
	box_blur_h(&cov[0][0], &tmp[0][0], W, H, GLOW_R);  box_blur_v(&tmp[0][0], &tmp[0][0], W, H, GLOW_R);
	box_blur_h(&tmp[0][0], &tmp[0][0], W, H, GLOW_R/2); box_blur_v(&tmp[0][0], &tmp[0][0], W, H, GLOW_R/2);
	/* 3) composite: sheet (gain 3, halved when unfocused) then core */
	uint32_t sheet = dark_ink ? 0x00f2f6fa : 0x0010151f;
	uint32_t core  = dark_ink ? 0x001a2330 : 0x00f0f4f8;
	int gain = focused ? 3 : 2;
	for (int yy = 0; yy < H; yy++) for (int xx = 0; xx < W; xx++) {
		int a = tmp[yy][xx] * gain; if (a > 255) a = 255;
		if (a) nw_over_pixel(sc, cx - W / 2 + xx, yy, ((uint32_t)a << 24) | sheet);
	}
	for (int yy = 0; yy < H; yy++) for (int xx = 0; xx < W; xx++) {
		int a = nw_cov143()[cov[yy][xx]]; if (!focused) a = (a * 200) >> 8;
		if (a) nw_over_pixel(sc, cx - W / 2 + xx, yy, ((uint32_t)a << 24) | core);
	}
}
```

Write `rasterize_run_coverage` as a sibling static using the same glyph walk as `nw_text` (UTF-8 decode if `nw_text` does it, same `nwfont_get(NWFONT_UI, cp)` and advance math) but `cov[y][x] = max(cov[y][x], glyphcov)`. Write `box_blur_h/v` as running-sum box blurs over the byte buffer (radius r: `out[i] = sum(in[i-r..i+r]) / (2r+1)`, clamped window at the ends). All buffers static — no allocation.

- [ ] **Step 4: Rewire the glass branch of draw_window_to**

Replace the key-black fill + 8-copy halo (lines ~112, 124–132):

```c
	if (s_glass_frame) {
		/* real-alpha ink canvas: transparent band, glow + core carry alpha */
		nw_clear_argb(sc, ox, oy, fw, fh, 0x00000000u);
	} else { ... unchanged ... }
	...
	if (s_glass_frame) {
		int dark_ink = (w->ink_dark != 0) && !dark;   /* dark windows always light core */
		draw_caption_glow(sc, ox + fw / 2, ty - oy, title, dark_ink, focused);
	} else { ... unchanged classic path ... }
```

(`draw_caption_glow` paints in frame-local coords when `ox/oy` are 0 — the frame-cache path; for the screen-space path pass `sc` offset the same way the current code does. Keep the guard that skips CPU control glyphs when `s_glass_frame`.)

- [ ] **Step 5: FS_WIN — band ink from real alpha**

In `nw_compose_gl.c`, replace:

```glsl
    float band_ink = smoothstep(0.02, 0.10, max(content.r, max(content.g, content.b)));
    float inkcov   = mix(band_ink, ctex.a, cin * u_inkwin);
```

with:

```glsl
    float inkcov = ctex.a * mix(1.0, u_inkwin, cin);   /* band: real alpha; client: alpha only for ink windows */
```

(Band pixels are nwm-authored ARGB after Step 4. Legacy clients keep `inkcov=0` in the client area and are forced opaque by the existing final `mix(lit, content, cin * (1.0 - u_inkwin))` — unchanged.)

- [ ] **Step 6: Build + gate + visual**

`make image64-gl -j8 && bash scripts/smoke-virtio-gpu-gl.sh` → `PASS`.
Visual screencapture checks:
- Title text has a soft, round glow sheet (no square ±1px fringing), dark text + light glow over the light wallpaper areas; if you drag a window over a dark wallpaper orb, after release+retitle-damage the polarity flips (dark sheet + light core) and does NOT flicker at the boundary (hysteresis).
- Terminal (dark window): light core + dark sheet always.
- Caption spheres unchanged (still shader-drawn, in the right slots).
- CPU fallback `NWM_NO_GL=1`: classic titlebars unchanged (glass branch is GL-only).
- `NWM_NO_GLASS=1`: no black frames (the `nw_gl_glass_on()` gate from 6185a2a keeps `s_glass_frame=0` — verify).

- [ ] **Step 7: Commit**

```bash
git add user/nwm/nw_compose.c user/nwm/nw_compose.h user/nwm/nwm_core.h user/nwm/nwm.c user/nwm/nw_compose_gl.c
git commit -m "nwm: Aero caption glow with auto ink polarity; frame band on real alpha ink (drops luminance keying)"
```

---

### Task 7: libnwui light-glass interiors

Give the toolkit a glass mode: transparent ink-canvas background, translucent widget fills (scrims), alpha-of-ink text hierarchy. Contract: `NWUI_GLASS` windows paint ARGB; every widget must cover its own footprint (dirty repaints don't re-clear).

**Files:**
- Modify: `user/libnwui/nwui.h`, `user/libnwui/nwui.c` (open-with-style + glass flag)
- Modify: `user/libnwui/nwui_paint.c` (clear + all widget background fills + text)

**Interfaces:**
- Consumes: `nw_create_window_style` (existing), `nw_clear_argb`/`nw_over_*`/`nw_text_argb` (Task 5).
- Produces (Task 8 and the Rust FFI use these):

```c
struct nwui *nwui_open_style(const char *title, int w, int h, uint32_t style); /* style = NW_STYLE_* */
/* nwui_open(title,w,h) stays and forwards with style 0 */
```

plus an internal `int glass;` on the `struct nwui` (set when `style & NW_STYLE_GLASS_CLIENT`).

- [ ] **Step 1: Style plumb**

`nwui_open_style` duplicates `nwui_open` (`nwui.c:90–108`) but calls `nw_create_window_style(d, w, h, title, style)` and sets `u->glass = !!(style & NW_STYLE_GLASS_CLIENT)`. `nwui_open` becomes `return nwui_open_style(title, w, h, 0);`.

- [ ] **Step 2: Glass palette + clear**

In `nwui_paint.c`, next to the existing `COL_*` constants, add the glass ink palette (mockup values):

```c
/* light-glass interiors (ARGB, straight alpha): ink #17222f on the slab */
#define GCOL_CANVAS   0x00000000u   /* fully transparent: the GL slab shows through */
#define GCOL_INK      0xff17222fu   /* primary ink */
#define GCOL_INK_SOFT 0x9e17222fu   /* secondary: same ink at 62% */
#define GCOL_SCRIM    0x40ffffffu   /* text-pane scrim: white 25% */
#define GCOL_FIELD    0x59ffffffu   /* input/list wells: white 35% */
#define GCOL_SEL      0x59ffffffu   /* selection pill: white 35% (mockup .side .sel) */
#define GCOL_SEL_RING 0x80ffffffu   /* selection inset hairline: white 50% */
#define GCOL_BTN_TOP  0x24ffffffu   /* button pill: white .14 -> .05 */
#define GCOL_BTN_BOT  0x0dffffffu
#define GCOL_BTN_RING 0x2effffffu   /* inset ring white .18 */
#define GCOL_SEP      0x2e17222fu   /* separators: ink 18% */
```

In `nwui_render` (line ~331), the clear becomes mode-dependent:

```c
	if (u->glass) nw_clear_argb(s, 0, 0, u->win_w, u->win_h, GCOL_CANVAS);
	else          nw_fill_rect(s, 0, 0, u->win_w, u->win_h, COL_WIN);
```

- [ ] **Step 3: Widget painters — glass variants of every background fill**

In `paint_self` (and `draw_menu` + the modal panel in `nwui_render`), branch each background fill on `u->glass` (thread `u` in — `paint_self` already receives the ui struct or the node carries a back-pointer; follow the existing access pattern). Exact mapping (opaque → glass):

| Widget / site | Today | Glass mode |
|---|---|---|
| window clear (`nwui_render:331`) | `nw_fill_rect COL_WIN` | `nw_clear_argb GCOL_CANVAS` |
| panel body (`:248`) | `nw_fill_round COL_PANEL_BG,235` | `nw_over_round GCOL_SCRIM` |
| panel header | `nw_fill_round COL_PANEL_HDR,255` | `nw_over_round` accent with alpha `0x66` (`(0x66<<24)\|accent`) |
| textarea (`:105`) / textfield (`:85`) / list (`:147`) / iconview (`:175`) wells | `nw_fill_round COL_TF_BG,255` | `nw_over_round GCOL_FIELD` |
| list/iconview selection | `nw_fill_round COL_SEL/ACCENT` | `nw_over_round GCOL_SEL` + `nw_stroke_round`→`nw_over_round` 1px `GCOL_SEL_RING` (fill the 1px ring by drawing the round rect twice: outer ring colour then inner fill inset by 1) |
| button normal (`:72`) | `nw_fill_round base,255` + sheen + stroke | `nw_over_round GCOL_BTN_TOP` top half + `GCOL_BTN_BOT` bottom half (two rects meeting mid-height, or one fill + darker bottom overlay) + 1px `GCOL_BTN_RING` |
| button pressed | `COL_BTN_DBOT` | same pill with `GCOL_BTN_TOP` alpha doubled (`0x48ffffff`) |
| sidebar link active pill (`:60–71`) | `nw_fill_round COL_ACCENT,255` | `nw_over_round GCOL_SEL` + ring (mockup `.side .sel`) |
| checkbox box (`:255`) | `nw_fill_round COL_TF_BG,255` | `nw_over_round GCOL_FIELD` |
| separators / borders | strokes with ink colours | `nw_over_*` with `GCOL_SEP` |
| context menu (`:306`) / modal panel (`:334`) | `nw_fill_round 0xf4f8fd,255` | keep NEARLY opaque on purpose: `nw_over_round 0xf0f7fafd` (94% white) — floating surfaces need contrast (research §5: text-dense floats sit on strong scrims) |
| modal dim (`:334`) | `nw_blend_rect 0,90` | `nw_over_rect 0x5a000000` |
| label text | `nw_text fg` | `nw_text_argb` with `GCOL_INK` (or `GCOL_INK_SOFT` when the node's colour is the current muted grey — map existing `COL_MUT`-style constants to the soft ink) |
| textarea/textfield/list TEXT | `nw_draw_char_t`/`nw_text` dark on white | unchanged colour BUT the well is `GCOL_FIELD` (35% white): keep ink `GCOL_INK` via the argb text path |
| iconview captions (on raw glass) | `nw_text` | `nw_text_argb GCOL_INK` + 1px soft dilate halo: draw the caption 4× at ±1px in `0x66f2f6fa` FIRST, then the core (cheap 4-neighbour halo — full §4d dilate is overkill at 12px captions) |
| icon PNG alpha blend (`:208–213`) | `nw_blend_pixel(..., p&0xffffff, a)` | `nw_over_pixel(s, x, y, (a<<24)\|(p&0xffffff))` so icon alpha lands in the canvas |
| icon drop shadow (`:203`) | `nw_fill_round(...,COL_ICON_SHADOW,34)` | `nw_over_round` with `0x22000000` |

Every glass-mode widget MUST paint its full footprint (the well fills do this) — verify no painter relies on the window clear for its background in the dirty-repaint path (`repaint_dirty:287–303`).

- [ ] **Step 4: Build + gate**

`make image64-gl -j8 && bash scripts/smoke-virtio-gpu-gl.sh` → `PASS`. No visual change yet (no app opts in until Task 8) — legacy apps must look IDENTICAL (style 0 → `u->glass=0` → all branches take the opaque path). Screencapture and compare Files/Settings/Notepad against a pre-task capture.

- [ ] **Step 5: Commit**

```bash
git add user/libnwui/nwui.h user/libnwui/nwui.c user/libnwui/nwui_paint.c
git commit -m "libnwui: light-glass mode - ARGB ink canvas, translucent widget scrims, alpha ink hierarchy"
```

---

### Task 8: Apps opt in — Settings, Notepad, About (C) + Files/rsexp (Rust)

**Files:**
- Modify: `user/settings/settings.c:210`, `user/notepad/notepad.c:165`, `user/about/about.c:53`
- Modify: `user/rust/libnwui-rs/src/lib.rs` (~lines 57, 204–206), `user/rust/rsexp/src/lib.rs:1259`

**Interfaces:**
- Consumes: `nwui_open_style` (Task 7), `NW_STYLE_GLASS_CLIENT` (=1, `nwproto.h:61`).

- [ ] **Step 1: C apps**

In each of settings.c/notepad.c/about.c replace `nwui_open("Title", W, H)` with `nwui_open_style("Title", W, H, NW_STYLE_GLASS_CLIENT)` (include the header that carries the constant — `nwui.h` should re-export it; if not, `#include "nwproto.h"` the way libnwui does).

- [ ] **Step 2: Rust FFI + Files**

`user/rust/libnwui-rs/src/lib.rs`: add the extern (next to the existing `nwui_open` decl at ~line 57):

```rust
fn nwui_open_style(title: *const c_char, w: i32, h: i32, style: u32) -> *mut c_void;
```

and a safe wrapper next to `Ui::open` (~line 204):

```rust
pub fn open_glass(title: &str, w: i32, h: i32) -> Option<Ui> { /* same as open() but calls nwui_open_style(..., 1) */ }
```

`user/rust/rsexp/src/lib.rs:1259`: `Ui::open("Files", 620, 420)` → `Ui::open_glass("Files", 620, 420)`.

- [ ] **Step 3: Build + gate + visual comparison against the mockup**

`make image64-gl -j8 && bash scripts/smoke-virtio-gpu-gl.sh` → `PASS`.
Boot `gl-desktop-show.sh`, open Files + Notepad + Settings + Terminal, screencapture, compare side-by-side with `scratch/glass-shots/design-mockup-full.png`:
- Files: sidebar + icon grid sit ON the glass slab (wallpaper reads through), selection = white 35% pill with hairline, folder icons pop, captions legible (halo).
- Notepad: text well = subtle white scrim over glass, ink `#17222f`, menus readable.
- Windows still drag at interactive speed (drag a window for ~3s: no visible lag versus before — the ARGB path costs more per pixel; if dragging visibly chugs, report as a concern, do not silently ship).
- CPU fallback `NWM_NO_GL=1`: the CPU compositor ignores the top byte, so a glass-styled app renders on a black canvas (transparent → 0x000000) with its ink RGB intact. Verify all text is readable. The black background is an accepted cosmetic limitation of the fallback — note it in the task report, do not engineer around it.

- [ ] **Step 4: Commit**

```bash
git add user/settings/settings.c user/notepad/notepad.c user/about/about.c user/rust/libnwui-rs/src/lib.rs user/rust/rsexp/src/lib.rs
git commit -m "apps: Files/Settings/Notepad/About opt into light-glass interiors"
```

---

### Task 9: Taskbar + top bar — dark glass chrome

The mockup's menubar/taskbar are dark glass (`rgba(12,17,28,.42)` / `.40`) with backdrop blur. The chrome layer is CPU-drawn and keyed over the GL scene, so: GL draws two dark-glass band quads (frost from the existing blur machinery), and the CPU chrome ink rides on top via the existing key.

**Files:**
- Modify: `user/nwm/nw_compose_gl.c` (new FS_BAR program + two quads in `nw_gl_frame` after windows, before chrome)
- Modify: `user/nwm/nw_compose.c` (`draw_taskbar` ~line 293 and the top-bar drawer: key-black backgrounds + bright ink in glass mode)

**Interfaces:**
- Consumes: `s_glass_frame` (CPU side), `g_blurB`/`backdrop()` machinery, taskbar geometry `NW_TASK_H` (`nwm_core.h`), top-bar height (find its constant where the top bar is drawn — grep `menubar\|topbar\|NW_TOP` in `nw_compose.c`).
- Produces: nothing downstream.

- [ ] **Step 1: FS_BAR shader** — a flat dark frost band (no bevel/lens):

```c
static const char *FS_BAR =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n"
    "varying vec2 v_uv;\n"
    "uniform highp vec4 u_rect;\n"
    "uniform sampler2D u_backdrop;\n"    /* the full-screen scene, blurred: see step 2 */
    "uniform vec2  u_screen;\n"
    "uniform float u_topline;\n"         /* +1: hairline at top edge (taskbar); -1: at bottom (menubar) */
    "void main() {\n"
    "    vec2 spx = u_rect.xy + v_uv * u_rect.zw;\n"
    "    vec3 c = texture2D(u_backdrop, vec2(spx.x / u_screen.x, 1.0 - spx.y / u_screen.y)).rgb;\n"
    "    float luma = dot(c, vec3(0.299, 0.587, 0.114));\n"
    "    c = mix(vec3(luma), c, 1.35);\n"                     /* saturate like the dark slab */
    "    c = mix(c, vec3(0.047, 0.067, 0.110), 0.42);\n"      /* rgba(12,17,28,.42) */
    "    float edge = (u_topline > 0.0) ? v_uv.y : (1.0 - v_uv.y);\n"
    "    c = mix(c, vec3(1.0), (1.0 - smoothstep(0.0, 1.5 / u_rect.w, edge)) * 0.15);\n"
    "    gl_FragColor = vec4(c, 1.0);\n"
    "}\n";
```

Blur source: the bars sit over the WHOLE composed scene. The per-window grab machinery is window-sized; do NOT add new FBOs. Cheapest correct option honouring the fork rule: sample the scene texture directly (sharp, `g_scene_tex` — it is already bound as the source for grabs) and let the dark tint + saturation do the material work; a bar reads as "smoked glass" rather than "frosted", which is acceptable and mockup-close (the mockup bars are thin — blur is barely visible through 42% dark tint). Bind `g_scene_tex` as `u_backdrop`. NOTE: sampling the scene FBO's own texture while rendering INTO that FBO is undefined — draw the two bar quads AFTER the scene is composited, in the present/scanout pass where the scene texture is read anyway (find where the scene is drawn to the screen — the same place the chrome overlay is keyed on — and insert the bar quads between scene present and chrome overlay).

- [ ] **Step 2: Program setup + draws**

Compile FS_BAR at init (`g_prog_bar`, uniforms `u_rect/u_backdrop/u_screen/u_topline` — same pattern as Task 3). In the present pass, after the scene quad, before the chrome overlay quad: draw the top-bar rect `(0, 0, screen_w, TOPBAR_H)` with `u_topline=-1.0` and the taskbar rect `(0, screen_h - NW_TASK_H, screen_w, NW_TASK_H)` with `u_topline=+1.0`. Use the actual top-bar height constant found in `nw_compose.c` (do not invent one; if the top bar is part of `draw_chrome`, read its height from the drawing code).

- [ ] **Step 3: CPU chrome ink in glass mode**

In `draw_taskbar` (`nw_compose.c:293`) and the top-bar drawer, when `s_glass_frame`: fill the bar background with key black `0x000000` (so the GL bar shows through the chrome key) and draw ink (button labels, clock, logo) in bright colours that clear the chrome key threshold — white `0xf0f5fc` text, button pills as bright hairline rings (`nw_stroke_round` with `0x66ffffff`-equivalent opaque colour `0xaab4c4` — chrome overlay has no alpha, brightness IS the alpha under the key). Active/hover button states: brighter fill `0x3a4456`→ keyed as translucent — verify against the key threshold used for the chrome overlay in `nw_compose_gl.c` (find the chrome key smoothstep; ink must clear its upper knee). When NOT `s_glass_frame`, the classic opaque taskbar path stays byte-identical.

- [ ] **Step 4: Build + gate + visual**

`make image64-gl -j8 && bash scripts/smoke-virtio-gpu-gl.sh` → `PASS`.
Visual: bars are dark smoked glass (desktop/orbs read through them darkly), top bar has a bottom hairline, taskbar a top hairline; button labels + clock crisp white; focused-window taskbar button visibly highlighted; windows slide UNDER the bars (bar quads draw after the scene). CPU fallback: classic opaque bars.

- [ ] **Step 5: Commit**

```bash
git add user/nwm/nw_compose_gl.c user/nwm/nw_compose.c
git commit -m "nwm-gl: dark-glass menubar + taskbar bands; chrome ink keyed over them"
```

---

### Task 10: Visual conformance pass + docs

Final tuning loop against the mockup, then documentation.

**Files:**
- Modify: `user/nwm/nw_compose_gl.c` (constant tweaks only), possibly `user/libnwui/nwui_paint.c` (palette tweaks only)
- Modify: `docs/en/graphics.md`, `docs/pl/graphics.md` (section 9.1 — extend with v3: lens, shadows, glow, glass interiors, bars)

**Interfaces:** none new.

- [ ] **Step 1: Produce the comparison sheet**

Boot `gl-desktop-show.sh`; arrange Files (focused) + Notepad (unfocused, overlapping) + Terminal (dark) roughly like the mockup stage; screencapture the QEMU window to `scratch/glass-shots/v3-desktop.png` (main tree path). Render the mockup reference if missing: headless Chrome `--screenshot` of the plan HTML (command in the main-tree scratch history; reference already at `scratch/glass-shots/design-mockup-full.png`).

- [ ] **Step 2: Tune to close the gap**

Compare the pair. Adjust ONLY numeric constants (tint alphas, sheen strength, caustic gain, shadow alpha, scrim alphas) for the largest visible deltas. Typical suspects: over-strong caustic (rim blows out on bright wallpaper), scrims too faint (interior looks flat), shadow too dark at 60%. Re-capture after each change; stop when the remaining differences are content (real apps vs lorem ipsum), not material. Keep a list of every constant changed and why — it goes in the task report.

- [ ] **Step 3: Docs**

Extend `docs/en/graphics.md` §9.1 (and the PL mirror) with: the lens recipe (profile/Snell/CA/caustic, constants and where they live), the shadow pass, the caption glow + polarity, the light-glass interior contract (`nwui_open_style` + ARGB primitives + palette), the bar quads, and the updated debug modes. One tight section, code-reference style, matching the existing doc voice.

- [ ] **Step 4: Full gates**

`make image64-gl -j8 && bash scripts/smoke-virtio-gpu-gl.sh` → `PASS`. Also run the plain build once (`make image64 -j8`) to be sure the non-GL image still links (libnw/libnwui changed).

- [ ] **Step 5: Commit**

```bash
git add user/nwm/nw_compose_gl.c user/libnwui/nwui_paint.c docs/en/graphics.md docs/pl/graphics.md
git commit -m "nwm-gl: conformance tuning against the aero-liquid-glass mockup + docs for glass v3"
```

---

## Task order & dependencies

1 → 2 → 3 touch only the GL shader file (serial). 4 → 5 → 6 build the ink pipeline (serial; 6 needs 4+5). 7 needs 5; 8 needs 7. 9 needs 6's CPU-key knowledge but is independent of 7/8. 10 last. Recommended execution order: **1, 2, 3, 4, 5, 6, 7, 8, 9, 10** (single implementer at a time — several tasks edit `nw_compose_gl.c`).

## Known risks (call out in reports, don't silently absorb)

- **Perf:** ARGB src-over has a per-pixel divide; app interiors are repaint-on-dirty so steady-state cost is low, but window DRAG re-composites (GPU side — unaffected). If interior repaint feels slow, report; do not micro-optimize unprompted.
- **CPU fallback cosmetics:** glass-styled apps on `NWM_NO_GL=1` show black canvases (top byte ignored, ink still readable). Accepted for now; note it.
- **Chrome key threshold (Task 9):** chrome ink brightness must clear the chrome overlay's key smoothstep — read the actual threshold from `nw_compose_gl.c` before picking ink colours.
- **`GLOW_MAXW` clamp:** titles longer than ~470px truncate in the glow buffer — matches the existing no-title-clamp Minor; fine.
