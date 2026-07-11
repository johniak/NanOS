# Liquid-Glass Windows Implementation Plan (v2 — whole-window slab)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**STATUS: IMPLEMENTED** (2026-07-11, branch `feat/liquid-glass`) — all 7 tasks landed: widened frame
band, padded backdrop grab + sharp sampler, the window style word through the wire protocol, the CPU
keyed glass-frame mode, the liquid-glass slab shader + caption spheres, the Terminal dark
glass-client showcase, and this docs pass. See "Known tradeoffs" below for what was deliberately
left out of scope (band-ink alpha, light-glass CLIENT apps in libnwui, half-res blur).

**v2 note:** this version supersedes the earlier draft of this file (Win7 frame around an
opaque client). Jan approved a revised design via an interactive HTML mockup on 2026-07-03. The
approved model: **the ENTIRE window is ONE slab of macOS-Liquid-Glass** (blur + saturation + tint +
edge refraction), and everything — title, controls, and the app's own content — is *ink drawn on
that glass*. Caption buttons are **macOS-style glass spheres** (yellow / green / red), placed **on
the RIGHT with red in the corner** (Windows placement, macOS look). Apps choose their slab tint at
window creation (light default, dark like the Terminal). Legacy apps that don't opt in keep an
opaque client area sitting on the slab.

**Goal:** every nwm window becomes a single liquid-glass slab: GPU blur + edge-lens refraction +
chromatic aberration + specular + hairlines; glass-sphere caption buttons rendered by the shader;
CPU chrome ink (glowing centred title) keyed on top; per-window tint (light/dark) and per-window
"glass client" mode where the app's pixels carry real ink alpha.

**Architecture:** builds on the committed GPU-native `nw_compose_gl.c` compositor (per-window
content textures, backdrop grab, FBO ping-pong Gaussian — commit 9df3d96 + the quality fixes).
Five changes: (1) padded backdrop grab so the lens can sample outside the window; (2) a window
STYLE word in the wire protocol (`NW_REQ_CREATE_WINDOW.c`) carrying `GLASS_CLIENT` and `DARK`
bits; (3) the CPU frame renderer gains a keyed glass mode — band = key black, centred glowing
title, NO control glyphs (spheres are GPU); (4) `FS_WIN` replaced by the liquid-glass shader
(full source in Task 5): rounded-box SDF → bevel refraction ring, three SDF glass spheres, tint,
ink compositing with two client modes (legacy opaque punch vs alpha-ink); (5) the Terminal opts
in as the dark glass-client showcase.

**Tech Stack:** GLSL ES 100 (GLES2), existing glkms/GBM/KMS scanout, CPU 2D toolkit (libnw) for chrome ink.

## Global Constraints

- **PRE-CONDITION:** the GL-desktop quality fixes are DONE first (drag-stutter upload storm Parts
  1a/1b/1c + the white-band root cause from `fable-fix-plan-gl-desktop-quality.md`). This plan
  multiplies per-frame GPU work; it must land on a compositor that uploads zero bytes on a
  move-only frame.
- Shaders are GLSL ES 100. No `dFdx/dFdy` (not core in ES2) — gradients by central differences of
  the SDF. No constant arrays (ES 100) — per-index selection uses chained ternaries. Fragment
  precision: `#ifdef GL_FRAGMENT_PRECISION_HIGH → highp` guard (SDF math at 1280 px overflows
  mediump quantization).
- NanOS surfaces are `0xAARRGGBB` with the top byte historically unused (=0). Sampled texel
  `.rgb = (B,G,R)` → always swizzle `.bgr`; the top byte arrives in `.a` (this plan starts using it
  as ink alpha for opted-in windows).
- The backdrop grab texture rows are **bottom-up** (copied from the default framebuffer); all
  backdrop sampling flips v once, inside the `backdrop()` helper. Never add a second flip.
- CPU fallback path (`NWM_NO_GL=1`, plain QEMU) must render EXACTLY as today whenever the keyed
  mode flag is off. The glass look is GL-only; the CPU path ignores the style word's visuals
  (dark still comes from the `"\x01"` title prefix there).
- Build flow: editing `user/nwm/*`, `user/libnw/*` or `user/terminal/*` requires `make nwm-gl`
  (Docker Mesa link) **then** `make image64-gl`. Plain `make image64` does NOT rebuild the GL nwm.
- Visual oracle = macOS `screencapture` of the QEMU window rect (`scripts/gl-desktop-show.sh`
  flow), including a capture DURING a window drag. The gl=es monitor `screendump` is blind — never
  the oracle. Automated gate = `bash scripts/smoke-virtio-gpu-gl.sh`.
- Reference for the intended look: Jan's approved mockup — slab (blur ~24 px, saturate ~1.65,
  light Aero-blue tint), edge lens ~14 px with faint cyan/warm fringes, diagonal specular, dark
  outer + white inner hairline, glass spheres 16 px (yellow/green/red-in-corner, grey when
  unfocused, specular dot + bottom caustic), centred glowing title, dark slab variant for the
  Terminal.
- Commit messages: plain, no AI/Claude attribution or Co-Authored-By trailers.

---

### Task 1: Widen the frame band (Aero metrics)

A 2 px border cannot read as glass. Widen to 6 px; the titlebar (28 px) already works.

**Files:**
- Modify: `user/nwm/nwm_core.h:22` (`NW_BORDER = 2` → `NW_BORDER = 6`)

**Interfaces:**
- Produces: `NW_BORDER == 6`, consumed by both compositors' `frame_w/frame_h`, hit tests, and
  resize logic (all already parameterized on the constant — grep confirms no hardcoded `2`).

- [x] **Step 1: Edit the constant**

```c
	NW_BORDER      = 6,     /* glass frame band: side/bottom border width */
```

- [x] **Step 2: CPU-path regression check**

Run: `make image64 && bash scripts/smoke-virtio-gpu.sh`
Expected: PASS. Then eyeball once via `make run64`: windows draggable/resizable, border visibly
thicker, nothing clipped.

- [x] **Step 3: Commit**

```bash
git add user/nwm/nwm_core.h
git commit -m "nwm: widen window border to 6px (glass frame band)"
```

---

### Task 2: Padded backdrop grab + sharp-backdrop sampler (no visual change)

The lens must sample content OUTSIDE the window rect (light bends around the edge), and the ring
needs an UNBLURRED backdrop. Grab a padded rect, keep `g_grab` (sharp) bound as a third sampler,
and map backdrop sampling through an explicit grab-rect uniform. After this task the desktop must
look IDENTICAL to before (parity refactor).

**Files:**
- Modify: `user/nwm/nw_compose_gl.c` (`blur_backdrop`, `FS_WIN`, uniforms, draw call)

**Interfaces:**
- Produces (file-static, consumed by Task 5): globals `g_gx, g_gy, g_gw, g_gh` (grab rect in
  screen px, top-down origin); uniform `u_grab` (vec4 = that rect); sampler unit 2 = sharp grab
  (`u_sharp`); `#define NW_GLASS_PAD 24`.

- [x] **Step 1: Pad the grab in `blur_backdrop` (and fix the latent resize bug)**

The current code has a latent bug: `ensure_tex(&g_grab, &g_blur_w, &g_blur_h, fw, fh)` updates
`g_blur_w/h` BEFORE the `g_blurA/B` size check reads them, so that check always passes and A/B are
never reallocated when a window is resized (stale-size FBO attachments → clipped blur). Replace
the whole function — all three textures managed together, size compared before realloc:

```c
#define NW_GLASS_PAD 24   /* px margin so refraction can sample outside the window */

static int g_gx, g_gy, g_gw, g_gh;   /* current grab rect, screen px, top-down origin */

/* GPU two-pass separable Gaussian of the PADDED fb region under the window into g_blurB.
 * The pad lets the glass shader refract content from just outside the frame. g_grab keeps the
 * sharp copy (sampled by the lens ring); g_blurB ends up with the frost. */
static void blur_backdrop(int wx, int wy, int fw, int fh)
{
	int gx0 = wx - NW_GLASS_PAD, gy0 = wy - NW_GLASS_PAD;
	int gx1 = wx + fw + NW_GLASS_PAD, gy1 = wy + fh + NW_GLASS_PAD;
	if (gx0 < 0) gx0 = 0;  if (gy0 < 0) gy0 = 0;
	if (gx1 > g_sw) gx1 = g_sw;  if (gy1 > g_sh) gy1 = g_sh;
	g_gx = gx0; g_gy = gy0; g_gw = gx1 - gx0; g_gh = gy1 - gy0;

	if (!g_grab || g_blur_w != g_gw || g_blur_h != g_gh) {   /* (re)alloc all three together */
		if (g_grab)  glDeleteTextures(1, &g_grab);
		if (g_blurA) glDeleteTextures(1, &g_blurA);
		if (g_blurB) glDeleteTextures(1, &g_blurB);
		g_grab  = make_tex(g_gw, g_gh, 0);
		g_blurA = make_tex(g_gw, g_gh, 0);
		g_blurB = make_tex(g_gw, g_gh, 0);
		g_blur_w = g_gw; g_blur_h = g_gh;
	}

	/* grab the padded backdrop from the default framebuffer (bottom-up rows) */
	glBindTexture(GL_TEXTURE_2D, g_grab);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_gx, g_sh - g_gy - g_gh, g_gw, g_gh);

	glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
	glViewport(0, 0, g_gw, g_gh);
	GLuint src = g_grab;
	for (int it = 0; it < 2; it++) {
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_blurA, 0);
		glUseProgram(p_blur.id);
		glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, src);
		glUniform1i(u_blur_tex, 0);
		glUniform2f(u_blur_dir, 1.0f / g_gw, 0.0f);
		quad_vp(&p_blur, 0, 0, g_gw, g_gh, g_gw, g_gh);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_blurB, 0);
		glBindTexture(GL_TEXTURE_2D, g_blurA);
		glUniform2f(u_blur_dir, 0.0f, 1.0f / g_gh);
		quad_vp(&p_blur, 0, 0, g_gw, g_gh, g_gw, g_gh);
		src = g_blurB;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, (int) g_kms.mode_w, (int) g_kms.mode_h);
}
```

**NOTE — reconcile with the current file:** if the attach-once fork rule (init-time `g_fboA/g_fboB`
pre-attached, sub-viewport blur) is in force in the current `blur_backdrop`, KEEP that resource
model and apply only the deltas from this step: the pad + `g_gx/g_gy/g_gw/g_gh` bookkeeping, the
sharp `g_grab` kept for the ring, and the size-compare-before-realloc fix (realloc at init or on
size change only — never mid-frame churn; screen-sized pre-attached targets with a `g_gw×g_gh`
sub-viewport satisfy that automatically).

**This also fixes a second latent bug in the current code (if still present):** `quad()` hardwires
`u_screen = (g_sw, g_sh)`, but the blur passes render into a `g_gw×g_gh` viewport — the NDC mapping
then covers only a corner of the FBO texture. The VS maps pixel rects against `u_screen`, so for a
full-viewport quad `u_screen` must equal the VIEWPORT size. Refactor `quad()` into an
explicit-viewport variant and keep the old signature as a wrapper (skip if the current code already
solved this with a VS_FULL full-viewport vertex shader — then use that for the blur passes):

```c
/* draw the unit quad through program p over the pixel rect (x,y,w,h), mapped against an
 * explicit target size (vw,vh) — MUST equal the current viewport (FBO passes ≠ screen). */
static void quad_vp(struct prog *p, float x, float y, float w, float h, float vw, float vh)
{
	glUseProgram(p->id);
	glUniform4f(p->rect, x, y, w, h);
	glUniform2f(p->screen, vw, vh);
	glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *) 0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
static void quad(struct prog *p, float x, float y, float w, float h)
{
	quad_vp(p, x, y, w, h, (float) g_sw, (float) g_sh);
}
```

The VS also flips y (`1.0 - px.y/screen.y*2.0`), so FBO output rows come out "upside-down"
relative to the screen convention — but the grab is ALSO upside-down (bottom-up copy) and the blur
is symmetric, so the two flips are self-consistent: the blurred texture keeps the grab's bottom-up
orientation and the shader's single `1.0 - v` flip in `backdrop()` remains correct for BOTH
samplers. Do not add extra flips.

- [x] **Step 2: Route FS_WIN's backdrop sampling through `u_grab`**

In `FS_WIN` (still the old shader for now), replace

```glsl
"  vec3 bd = texture2D(u_backdrop, vec2(v_uv.x, 1.0 - v_uv.y)).bgr;\n"
```

with

```glsl
"  vec2 spx = u_rect.xy + v_uv * u_rect.zw;\n"          /* fragment in screen px */
"  vec2 guv = (spx - u_grab.xy) / u_grab.zw;\n"
"  vec3 bd = texture2D(u_backdrop, vec2(guv.x, 1.0 - guv.y)).bgr;\n"
```

and add to FS_WIN's declarations (`u_rect` is the SAME program-scope uniform the VS uses —
declaring it in the FS is legal and reads the same value):

```glsl
"uniform vec4 u_rect;\n"
"uniform vec4 u_grab;\n"
```

- [x] **Step 3: Wire the new uniform + sharp sampler in C**

Add cached locations (next to the other `u_win_*`):

```c
static GLint u_win_grab, u_win_sharp;
```

in `nw_gl_init` after the existing `u_win_*` lookups:

```c
	u_win_grab  = glGetUniformLocation(p_win.id, "u_grab");
	u_win_sharp = glGetUniformLocation(p_win.id, "u_sharp");
```

(`u_sharp` doesn't exist in the shader until Task 5; `glGetUniformLocation` returning -1 is
harmless and `glUniform*` on -1 is a no-op.)

In the window draw block of `nw_gl_frame`, after the existing per-window uniform sets:

```c
		glUniform4f(u_win_grab, (float) g_gx, (float) g_gy, (float) g_gw, (float) g_gh);
		glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, w->glass ? g_grab : g_win_tex[idx]);
		glUniform1i(u_win_sharp, 2);
		glActiveTexture(GL_TEXTURE0);
```

- [x] **Step 4: Parity check**

Run: `make nwm-gl && make image64-gl && bash scripts/smoke-virtio-gpu-gl.sh`
Expected: PASS, and a `scripts/gl-desktop-show.sh` screencapture that looks the same as before
this task (same blur under windows — the pad only enlarges the blurred region, the mapping change
compensates exactly).

- [x] **Step 5: Commit**

```bash
git add user/nwm/nw_compose_gl.c
git commit -m "nwm-gl: padded backdrop grab + explicit grab-rect mapping + sharp sampler plumbing"
```

---

### Task 3: Window style word (per-app tint + glass-client opt-in) through the protocol

Apps must be able to say, at window creation: "my slab is dark" and/or "my client pixels carry ink
alpha". `NW_REQ_CREATE_WINDOW`'s `c` field is unused (libnw sends 0) — it becomes the style word.
No wire-format change, old clients keep working (style 0 = legacy light opaque).

**Files:**
- Modify: `user/libnw/nwproto.h` (style enum + comment)
- Modify: `user/libnw/libnw.h`, `user/libnw/libnw.c` (`nw_create_window_style`)
- Modify: `user/nwm/nwm_core.h` (`struct nw_window` gains `style`)
- Modify: `user/nwm/nwm_core.c` (CREATE_WINDOW stores it)

**Interfaces:**
- Produces: `NW_STYLE_GLASS_CLIENT = 1`, `NW_STYLE_DARK = 2` (nwproto.h);
  `nw_win *nw_create_window_style(nw_display *d, int w, int h, const char *title, uint32_t style);`
  (libnw); `w->style` on the server (consumed by Task 5's shader wiring and Task 6).
- Pixel contract produced here, consumed by Tasks 5/6: for a `NW_STYLE_GLASS_CLIENT` window the
  client buffer is `0xAARRGGBB` — top byte = ink coverage (0 = pure glass, 255 = opaque ink).
  Transport already preserves it end-to-end (verified in Step 3).

- [x] **Step 1: Protocol — style bits + comment**

In `user/libnw/nwproto.h`, update the CREATE_WINDOW comment and add the enum (below the
`NW_BTN_*` enum):

```c
	NW_REQ_CREATE_WINDOW  = 2,   /* a=w b=h c=style (NW_STYLE_*); payload=title             */
```

```c
/* Window style bits, sent in NW_REQ_CREATE_WINDOW.c. Style 0 = legacy: light glass slab with an
 * opaque client area. */
enum {
	NW_STYLE_GLASS_CLIENT = 1,   /* client pixels are 0xAARRGGBB: top byte = ink alpha over glass */
	NW_STYLE_DARK         = 2    /* dark slab tint (formal form of the "\x01" title prefix)       */
};
```

- [x] **Step 2: libnw client API**

`user/libnw/libnw.h`, next to `nw_create_window`:

```c
nw_win *nw_create_window_style(nw_display *d, int w, int h, const char *title, uint32_t style);
```

`user/libnw/libnw.c`: rename the existing body, thread `style` into the header's `c` field, and
keep the old name as a wrapper:

```c
nw_win *nw_create_window_style(nw_display *d, int w, int h, const char *title, uint32_t style)
{
	if (w <= 0 || h <= 0)
		return 0;
	int tl = title ? (int) strlen(title) : 0;
	if (send_hdr(d->reqfd, NW_REQ_CREATE_WINDOW, 0, w, h, (int32_t) style, 0, (uint32_t) tl) < 0)
		return 0;
	/* ... rest of the existing nw_create_window body, unchanged ... */
}

nw_win *nw_create_window(nw_display *d, int w, int h, const char *title)
{
	return nw_create_window_style(d, w, h, title, 0);
}
```

(Check `send_hdr`'s parameter order in the file — the existing call passes `w, h, 0, 0`; the style
replaces the first `0`, i.e. the `c` argument.)

- [x] **Step 3: Server side — store it, and verify the alpha transport**

`user/nwm/nwm_core.h`, in `struct nw_window` (next to `glass`):

```c
	uint32_t  style;       /* NW_STYLE_* bits from CREATE_WINDOW (tint, glass-client ink)     */
```

`user/nwm/nwm_core.c`, in the `NW_REQ_CREATE_WINDOW` case (next to `w->glass = 1;`):

```c
		w->style  = (uint32_t) m->c;         /* NW_STYLE_* (0 = legacy light/opaque)               */
```

(`nwm_core.h` must see the enum — it already includes the protocol header for `struct nw_msg`; if
not, add `#include "../libnw/nwproto.h"` the same way nwm_core.c reaches it.)

Then VERIFY (read, don't guess) that the top byte survives the pixel path end-to-end:
1. `commit_rect` (nwm_core.c) copies whole `uint32_t`s — already verbatim, OK.
2. `nw_blit` (libnw/nw_gfx.c), used by `draw_window_to` to place the client buffer into
   `w->frame` — confirm it copies words verbatim (no `& 0x00FFFFFF` masking). If it masks, remove
   the mask.
3. The GL upload of `w->frame` (nw_compose_gl.c) is `GL_RGBA/GL_UNSIGNED_BYTE` — byte 3 lands in
   `.a` in the shader. Nothing to change, just confirm the format.

- [x] **Step 4: Regression test**

Run: `make image64 && bash scripts/smoke-virtio-gpu.sh`
Expected: PASS — every existing client sends style 0 via the wrapper; behaviour identical.

- [x] **Step 5: Commit**

```bash
git add user/libnw/nwproto.h user/libnw/libnw.h user/libnw/libnw.c user/nwm/nwm_core.h user/nwm/nwm_core.c
git commit -m "nwm: window style word (dark tint + glass-client ink) in CREATE_WINDOW"
```

---

### Task 4: CPU keyed glass-frame mode (mechanism only, default OFF)

For the shader to put GPU glass UNDER the CPU chrome ink, the CPU must paint the frame band as
pure key black with only the ink on top. In the approved design the title is CENTRED with a soft
white halo (Aero glow) and there are NO CPU control glyphs — the caption spheres are drawn by the
shader (Task 5) at the exact hit-test slots that already exist (`close_box`/`max_box`/`min_box` in
nwm_core.c — right side, close in the corner; unchanged). Behind a flag; nothing changes while the
flag is off.

**Files:**
- Modify: `user/nwm/nw_compose.c` (`draw_window_to`, new setter)
- Modify: `user/nwm/nw_compose.h` (declaration)

**Interfaces:**
- Produces: `void nw_compose_set_glass_frame(int on);` — when on, `draw_window_to` renders the
  frame band as key black `0x000000` with only title ink on top; client area unchanged. Consumed
  by `nwm.c` in Task 5.
- Key contract with the shader: in the BAND, **pure black = glass**, any pixel with max channel
  above ~0.10 = ink over glass. Ink colours must clear that threshold (both title colours below
  do).

- [x] **Step 1: The flag + setter in `nw_compose.c`**

```c
/* GL liquid-glass windows: when set (GL compositor live), the frame band (titlebar + borders) is
 * rendered as pure key black with only the ink (centred glowing title) on top; the GL shader puts
 * the glass slab under that ink and draws the caption spheres itself. OFF = classic opaque CPU
 * frame (fallback path unchanged). */
static int s_glass_frame = 0;
void nw_compose_set_glass_frame(int on) { s_glass_frame = on ? 1 : 0; }
```

Declaration in `nw_compose.h` (next to `nw_compose_set_theme`):

```c
void nw_compose_set_glass_frame(int on);   /* GL glass windows: key-black band + ink (nw_compose_gl) */
```

- [x] **Step 2: Branch `draw_window_to`**

Replace the material + titlebar fills (`nw_compose.c:104-108`) with:

```c
	if (s_glass_frame) {
		nw_fill_rect(sc, ox, oy, fw, fh, 0x000000);            /* key: glass everywhere... */
	} else {
		nw_fill_rect(sc, ox, oy, fw, fh, mat);                 /* material */
		nw_vgrad_rect(sc, ox, oy, fw, NW_TITLEBAR_H,           /* title bar */
		              dark ? COL_TB_DTOP : COL_TB_TOP, dark ? COL_TB_DBOT : COL_TB_BOT);
		if (!focused)                                          /* dim the bar when unfocused */
			nw_blend_rect(sc, ox, oy, fw, NW_TITLEBAR_H, mat, 80);
	}
```

(The unfocused dim MUST be skipped in keyed mode — blending grey over the key would turn the whole
band into "ink". Unfocused appearance lives in the shader, Task 5.)

Replace the title-text block (the `tfg`/`ty`/dot/`nw_text` lines) with:

```c
	uint32_t tfg = dark ? COL_TITLE_DFG : COL_TITLE_FG;
	int ty = oy + (NW_TITLEBAR_H - NW_FONT_H) / 2;
	if (s_glass_frame) {
		/* centred title with a soft white halo (Aero glow) — ink over the GPU glass slab.
		 * Halo 0xdfe9f4 and core 0x223041 both clear the shader's ink threshold. */
		int tw = nw_text_w(title);
		int tx = ox + (fw - tw) / 2;
		for (int hy = -1; hy <= 1; hy++)
			for (int hx = -1; hx <= 1; hx++)
				if (hx || hy) nw_text(sc, tx + hx, ty + hy, title, 0xdfe9f4);
		nw_text(sc, tx, ty, title, 0x223041);
	} else {
		nw_fill_round(sc, ox + 10, ty + 2, 12, 12, 3, focused ? s_accent : 0x9fb2cc, 255);
		nw_text(sc, ox + 28, ty, title, tfg);
	}
```

Wrap the control-glyph block (`— □ ×`, from `uint32_t cfg = ...` through the close-× loop) in:

```c
	if (!s_glass_frame) {
		/* ...existing — □ × drawing unchanged... */
	}
```

The client blit and the no-buffer `nw_fill_rect(..., mat)` fallback at the end stay as-is.

- [x] **Step 3: Off-flag regression test**

Run: `make image64 && bash scripts/smoke-virtio-gpu.sh`
Expected: PASS — flag defaults to 0, CPU rendering byte-identical.

- [x] **Step 4: Commit**

```bash
git add user/nwm/nw_compose.c user/nwm/nw_compose.h
git commit -m "nwm: keyed glass-frame mode for the CPU frame renderer (mechanism, default off)"
```

---

### Task 5: The liquid-glass slab shader + caption spheres + wiring

The payoff. Replaces `FS_WIN` wholesale. Read the physics notes before touching the code — every
term is there for a reason and the debug modes let you SEE each one.

**Physics recap:** the whole window is a slab of glass with a curved bevel `BEVEL` px wide at the
outer edge. A rounded-box SDF `d` gives distance to the edge; `rim = 1 - clamp(-d/BEVEL)` is 1 at
the very edge, 0 on the flat body. The SDF gradient `g` (central differences — ES 100 has no dFdx)
points outward and IS the 2D surface-normal direction of the bevel. Refraction displaces the
backdrop sample OUTWARD along `g`, growing ~quadratically toward the rim (`rim²`), so the rim
shows a compressed sliver of what lies just outside the window — the "lensing". Chromatic
aberration = R/B sampled at ±8% of the displacement. The flat body shows the BLURRED backdrop
(frost); the rim ring mixes toward the SHARP refracted sample so the lens looks like clear bent
glass. Specular = the bevel facing the top-left light glints. Hairlines (1 px dark outer, 1 px
white inner) sell the edge. The three caption spheres repeat the same trick at small scale: each
is a mini-lens (refracted sharp backdrop) tinted with its colour — denser toward its rim like real
coloured glass — plus a specular dot up-left and a caustic (light focused through the ball) at the
bottom.

**Files:**
- Modify: `user/nwm/nw_compose_gl.c` (FS_WIN → liquid glass; uniforms; draw call; init; debug env)
- Modify: `user/nwm/nwm.c` (enable keyed mode on GL success; disable + re-dirty on GL fallback)

**Interfaces:**
- Consumes: Task 2's `u_grab`/`u_sharp`/pad; Task 3's `w->style` + pixel contract; Task 4's
  `nw_compose_set_glass_frame`.
- Produces: uniforms `u_client` (vec4, window-local px), `u_caps` (vec4: close-sphere centre x,y,
  radius, slot width), `u_focus`, `u_dark`, `u_inkwin`, `u_debug` (floats); env
  `NWM_GLASS_DEBUG=1..4`.

- [x] **Step 1: Replace `FS_WIN` with the liquid-glass shader**

Full source. Transcribe EXACTLY (as a C string array like the current shaders; comments may be
dropped in the C literal):

```glsl
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
varying vec2 v_uv;
uniform vec4 u_rect;          /* window rect, screen px (program-scope, shared with the VS)   */
uniform sampler2D u_content;  /* unit 0: CPU frame render; band=key black, client=app pixels  */
uniform sampler2D u_backdrop; /* unit 1: BLURRED padded backdrop grab (rows bottom-up)        */
uniform sampler2D u_sharp;    /* unit 2: SHARP padded backdrop grab (rows bottom-up)          */
uniform vec4  u_grab;         /* grab rect, screen px: x,y,w,h                                */
uniform float u_glass;        /* 1 = liquid-glass window, 0 = plain opaque window             */
uniform vec2  u_size_px;      /* frame w,h in px                                              */
uniform float u_radius_px;    /* corner radius                                                */
uniform vec4  u_client;       /* client rect, WINDOW-LOCAL px: x,y,w,h                        */
uniform vec4  u_caps;         /* caption spheres: close centre x,y; radius; slot width (px)   */
uniform float u_focus;        /* 1 focused, 0 not                                             */
uniform float u_dark;         /* 1 = dark slab variant                                        */
uniform float u_inkwin;       /* 1 = client pixels carry ink alpha; 0 = legacy opaque client  */
uniform float u_debug;        /* 0 off; 1 rim; 2 |offset|; 3 sharp grab; 4 blurred grab       */

const float BEVEL   = 14.0;   /* px over which the glass edge curves                          */
const float REFRACT = 12.0;   /* max lens displacement at the rim, px (focused)               */
const vec2  LIGHT   = vec2(-0.555, -0.832);  /* toward the light, screen coords (top-left)    */

float sd_box(vec2 p, vec2 b, float r) {      /* signed distance, rounded box centred at 0     */
    vec2 q = abs(p) - b + vec2(r);
    return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
}
vec3 backdrop(sampler2D t, vec2 spx) {       /* sample a grab texture at a SCREEN-px point    */
    vec2 uv = (spx - u_grab.xy) / u_grab.zw;
    return texture2D(t, vec2(uv.x, 1.0 - uv.y)).bgr;   /* grab rows are bottom-up */
}
void main() {
    vec2  p    = v_uv * u_size_px;                       /* window-local px, top-down */
    vec2  pc   = p - 0.5 * u_size_px;
    vec2  hb   = 0.5 * u_size_px;
    float d    = sd_box(pc, hb, u_radius_px);            /* < 0 inside the rounded window */
    float mask = clamp(0.5 - d, 0.0, 1.0);               /* 1px AA edge */
    if (mask <= 0.0) discard;

    vec4 ctex    = texture2D(u_content, v_uv);
    vec3 content = ctex.bgr;
    if (u_glass < 0.5) { gl_FragColor = vec4(content, mask); return; }

    /* client rect coverage: 1 inside, 0 in the frame band, 1px AA seam */
    vec2  cd  = max(u_client.xy - p, p - (u_client.xy + u_client.zw));
    float cin = 1.0 - clamp(max(cd.x, cd.y) + 0.5, 0.0, 1.0);

    /* ---- the slab ---- */
    float rim = 1.0 - clamp(-d / BEVEL, 0.0, 1.0);       /* 1 at the edge, 0 on the body */

    float e = 1.0;                                        /* SDF gradient: points OUTWARD */
    vec2 g = vec2(sd_box(pc + vec2(e, 0.0), hb, u_radius_px) - sd_box(pc - vec2(e, 0.0), hb, u_radius_px),
                  sd_box(pc + vec2(0.0, e), hb, u_radius_px) - sd_box(pc - vec2(0.0, e), hb, u_radius_px));
    g = normalize(g + vec2(1e-4));

    float refr   = REFRACT * mix(0.45, 1.0, u_focus);     /* unfocused: gentler lens */
    vec2  off_px = g * (rim * rim) * refr;                /* outward, quadratic toward the rim */

    vec3 ring;                                            /* chromatic aberration on the lens */
    ring.r = backdrop(u_sharp, u_rect.xy + p + off_px * 0.92).r;
    ring.g = backdrop(u_sharp, u_rect.xy + p + off_px       ).g;
    ring.b = backdrop(u_sharp, u_rect.xy + p + off_px * 1.08).b;
    vec3 body  = backdrop(u_backdrop, u_rect.xy + p);     /* frosted flat body */
    vec3 glass = mix(body, ring, smoothstep(0.15, 0.8, rim));

    float luma = dot(glass, vec3(0.299, 0.587, 0.114));   /* vibrancy + frost lift */
    glass = mix(vec3(luma), glass, 1.22);
    glass = glass * 0.90 + vec3(0.085);

    vec3  tcol = mix(vec3(0.76, 0.78, 0.81), vec3(0.46, 0.60, 0.80), u_focus);
    float tamt = mix(0.34, 0.22, u_focus);                /* unfocused: paler, denser tint */
    tcol  = mix(tcol, vec3(0.10, 0.11, 0.13), u_dark);
    tamt  = mix(tamt, 0.48, u_dark);                      /* dark glass is denser */
    glass = mix(glass, tcol, tamt);

    float facing = max(dot(g, LIGHT), 0.0);               /* specular glint on the lit bevel */
    glass += vec3(pow(facing, 3.0) * rim * rim * (0.35 + 0.25 * u_focus));

    float outer = 1.0 - clamp(-d - 0.5, 0.0, 1.0);        /* 1px dark outer hairline */
    float inner = clamp(-d - 1.0, 0.0, 1.0) * (1.0 - clamp(-d - 2.2, 0.0, 1.0)); /* 1px white inner */
    glass = mix(glass, vec3(0.13, 0.16, 0.20), outer * 0.55);
    glass += vec3(inner * 0.22);

    /* ---- caption spheres: yellow / green / red-in-corner glass balls ---- */
    for (int i = 0; i < 3; i++) {
        vec2  c = vec2(u_caps.x - float(2 - i) * u_caps.w, u_caps.y);
        vec2  q = p - c;
        float r = length(q);
        float R = u_caps.z;
        if (r < R + 1.0) {
            vec3 ccol = (i == 0) ? vec3(1.00, 0.74, 0.18)
                      : (i == 1) ? vec3(0.16, 0.78, 0.25)
                                 : vec3(1.00, 0.37, 0.34);
            ccol = mix(vec3(0.72, 0.76, 0.81), ccol, u_focus);        /* unfocused: grey glass */
            vec2  n  = q / R;
            float rr = clamp(r / R, 0.0, 1.0);
            vec3 ball = backdrop(u_sharp, u_rect.xy + p + n * rr * rr * 5.0);  /* mini-lens */
            ball = mix(ball, ccol, 0.42 + 0.38 * smoothstep(0.45, 1.0, rr));   /* rim-dense colour */
            ball *= 0.78 + 0.42 * (1.0 - rr);                         /* bright core, darker rim */
            vec2 sq = q - R * vec2(-0.30, -0.42);
            vec2 cq = q - R * vec2( 0.00,  0.55);
            ball += vec3(exp(-dot(sq, sq) / (R * R * 0.10)) * 0.90);  /* specular dot, up-left */
            ball += vec3(exp(-dot(cq, cq) / (R * R * 0.16)) * 0.35);  /* caustic, bottom */
            ball += vec3(smoothstep(R - 1.6, R - 0.4, r)
                         * (1.0 - smoothstep(R - 0.4, R + 0.6, r)) * 0.35);   /* rim ring */
            float cov = 1.0 - smoothstep(R - 0.6, R + 0.6, r);        /* AA edge */
            glass = mix(glass, ball, cov);
        }
    }

    /* ---- ink over glass ---- */
    /* band: CPU ink keyed on black (title glow). client: legacy = opaque punch; ink-window =
     * the app's own alpha (top byte) composites its pixels over the slab. */
    float band_ink = smoothstep(0.02, 0.10, max(content.r, max(content.g, content.b)));
    float inkcov   = mix(band_ink, ctex.a, cin * u_inkwin);
    vec3  lit      = mix(glass, content, inkcov);
    vec3  col      = mix(lit, content, cin * (1.0 - u_inkwin));

    if (u_debug > 0.5) {
        if      (u_debug < 1.5) col = vec3(rim, 0.0, 0.0);
        else if (u_debug < 2.5) col = vec3(length(off_px) / max(refr, 0.001));
        else if (u_debug < 3.5) col = backdrop(u_sharp,    u_rect.xy + p);
        else                    col = backdrop(u_backdrop, u_rect.xy + p);
    }
    gl_FragColor = vec4(col, mask);
}
```

Sign conventions, so you can reason instead of guess:
- `g` points OUTWARD (SDF grows outward). `off_px` outward ⇒ the rim shows a COMPRESSED sliver of
  content from just outside the window (thick-glass look). If the rim ever looks "inside-out"
  (stretched interior), the sign flipped somewhere — don't tune constants, fix the sign.
- Screen y grows DOWN; the grab texture rows are bottom-up; `backdrop()` does the single flip.
  Displacements are in screen px and pass through `backdrop()` untouched — do NOT add another flip.
- `dot(g, LIGHT)`: top edge has `g=(0,-1)`, `LIGHT` points up-left ⇒ top/left bevels glint,
  bottom/right stay dark. Correct as written.
- Ink-mode truth table (worth 30 s of checking after any edit): legacy window (`u_inkwin=0`) —
  band: glass+keyed ink, client: opaque content. Ink window (`u_inkwin=1`) — band: glass+keyed
  ink, client: `mix(glass, content, ctex.a)`. The spheres sit under the ink but the CPU draws no
  glyphs there (Task 4), so band ink never covers them.

- [x] **Step 2: Uniform wiring in C**

New cached locations (drop `u_win_alpha` and `u_win_border` if now unused by the new shader — they
are; delete their lookups and sets):

```c
static GLint u_win_client, u_win_caps, u_win_focus, u_win_dark, u_win_inkwin, u_win_debug;
static float g_debug;
```

`nw_gl_init` additions:

```c
	u_win_client = glGetUniformLocation(p_win.id, "u_client");
	u_win_caps   = glGetUniformLocation(p_win.id, "u_caps");
	u_win_focus  = glGetUniformLocation(p_win.id, "u_focus");
	u_win_dark   = glGetUniformLocation(p_win.id, "u_dark");
	u_win_inkwin = glGetUniformLocation(p_win.id, "u_inkwin");
	u_win_debug  = glGetUniformLocation(p_win.id, "u_debug");
	{ const char *dbg = getenv("NWM_GLASS_DEBUG"); g_debug = dbg ? (float) atoi(dbg) : 0.0f; }
```

Window draw block in `nw_gl_frame` — replace the old `dark`/`alpha` lines
(`int dark = (w->title[0] == '\x01'); float alpha = ...`) and the `u_win_alpha`/`u_win_border`
sets with (`fw`/`fh` are the frame size ints already computed in that block; `NW_CLOSE` comes from
nwm_core.h, the style bits from nwproto.h — both already reachable via the existing includes,
add `#include "../libnw/nwproto.h"` if the compiler disagrees):

```c
		int dark = (w->title[0] == '\x01') || (w->style & NW_STYLE_DARK);
		glUniform4f(u_win_client, (float) NW_BORDER, (float) NW_TITLEBAR_H,
		            (float) w->cw, (float) w->ch);
		glUniform4f(u_win_caps, (float) fw - NW_BORDER - 2.0f - NW_CLOSE * 0.5f,
		            NW_TITLEBAR_H * 0.5f, 8.0f, (float) NW_CLOSE);
		glUniform1f(u_win_focus, idx == s->focus ? 1.0f : 0.0f);
		glUniform1f(u_win_dark, dark ? 1.0f : 0.0f);
		glUniform1f(u_win_inkwin, (w->style & NW_STYLE_GLASS_CLIENT) ? 1.0f : 0.0f);
		glUniform1f(u_win_debug, g_debug);
```

The sphere centres land exactly in the existing `close_box`/`max_box`/`min_box` hit slots
(`cx = frame_w - NW_BORDER - NW_CLOSE - 2` + half a slot), so clicking the red ball closes, the
green maximizes, the yellow minimizes — with zero hit-test changes. Sphere radius 8 px inside the
22 px slot matches the approved 16 px mockup balls.

- [x] **Step 3: Enable keyed frames from nwm.c (and disable on fallback)**

At the GL-init success site (`user/nwm/nwm.c:793`, inside the `if (... nw_gl_init(...) == 0)`
block):

```c
		nw_compose_set_glass_frame(1);
```

The GL fallback lives in `present_gl()` at `user/nwm/nwm.c:545-551` (the `nw_gl_frame(...) != 0`
branch that logs "GL backend disabled, CPU fallback"). Add inside it, right after `g_gl = 0;`:

```c
		nw_compose_set_glass_frame(0);
		for (int i = 0; i < NW_MAX_WINDOWS; i++)     /* keyed frames are unusable on the CPU path */
			if (S.win[i].used) S.win[i].frame_dirty = 1;
		S.dirty = 1;                                 /* force a re-render + full CPU repaint */
```

(the server is the global `S` in nwm.c). Without this, a mid-session GL fallback would paint
key-black frames on the CPU path.

- [x] **Step 4: Build + shader-compile check**

Run: `make nwm-gl && make image64-gl`
Expected: builds clean. Boot `bash scripts/gl-desktop-show.sh`, log in, check serial for
`nw_gl: shader compile failed` — must NOT appear (if it does, the info-log names the line; ES 100
gotchas: all `float` literals need decimals, no constant arrays, chained ternaries need parens on
some compilers — add them if the log complains).

- [x] **Step 5: Visual verification via debug modes (screencapture each)**

Run nwm with the env knob from a VT shell (`NWM_GLASS_DEBUG=1 nwm`), screencapture per mode:
1. `=1`: windows show a red band exactly `BEVEL` px wide hugging the rounded edge, black
   elsewhere. Uniform width along all four edges and corners.
2. `=2`: same band as a white→black ramp (displacement magnitude), zero in the body.
3. `=3` / `=4`: the window region shows the sharp / blurred underlying desktop, aligned with
   what's actually behind the window (drag the window: the image must track 1:1, no offset drift —
   this validates the `u_grab` mapping).
4. Unset — the real material, checked against the approved mockup: frost + tint over the WHOLE
   window; a clear "lens" ring at the outer ~14 px showing bent outside content; faint colour
   fringing on the ring; glint along top/left; dark+white hairlines; centred title with a white
   glow; three glass balls right of centre with red in the corner (specular dot up-left, bright
   caustic at the bottom, colour denser at their rims); unfocused windows paler with GREY balls;
   legacy clients opaque and legible.

- [x] **Step 6: Gate + commit**

Run: `bash scripts/smoke-virtio-gpu-gl.sh` and the Part-0 interaction scenario (open two apps,
drag one across the other for ~2 s, screencapture DURING the drag) — frame telemetry must stay
within the drag budget established by the quality-fix work.
Expected: PASS.

```bash
git add user/nwm/nw_compose_gl.c user/nwm/nwm.c
git commit -m "nwm-gl: liquid-glass slab windows — SDF bevel refraction, glass-sphere captions, keyed chrome ink"
```

---

### Task 6: Terminal as the dark glass-client showcase

The approved mockup's Terminal: dark slab, shell text floating directly on the glass. The
terminal's cell renderer writes pixels verbatim (`nw_draw_char` fast path writes `fg`/`bg`
`uint32_t`s untouched, `nw_fill_rect` too), so ink alpha is just a matter of ORing it into the
palette colours at the call sites. The CPU fallback ignores the top byte — zero visual change
there.

**Files:**
- Modify: `user/terminal/terminal.c`

**Interfaces:**
- Consumes: `nw_create_window_style` + `NW_STYLE_*` (Task 3), the alpha-ink shader path (Task 5).

- [x] **Step 1: Opt in at window creation**

Replace `user/terminal/terminal.c:191`:

```c
	g_win = nw_create_window_style(d, 560, 360, "\x01" "Terminal",   /* \x01 keeps CPU-path dark */
	                               NW_STYLE_GLASS_CLIENT | NW_STYLE_DARK);
```

(`NW_STYLE_*` comes from nwproto.h via libnw.h; if libnw.h doesn't re-export it, add
`#include "../libnw/nwproto.h"`.)

- [x] **Step 2: Ink alpha in the palette**

Add next to the existing `vt_pal` uses:

```c
/* GL glass-client: the top byte is ink alpha. Text and coloured cells are solid ink; the DEFAULT
 * background (palette 0) is a thin dark veil so the glass slab shows through. The CPU fallback
 * ignores the top byte, so this changes nothing there. */
static uint32_t pal_fg(int idx) { return vt_pal(idx) | 0xff000000u; }
static uint32_t pal_bg(int idx)
{
	uint32_t c = vt_pal(idx);
	return idx == 0 ? (c | 0x50000000u) : (c | 0xff000000u);
}
```

Then replace every rendering call site (`grep -n "vt_pal" user/terminal/terminal.c` — currently
lines 52, 72-73, 111, 200): background fills `vt_pal(0)` → `pal_bg(0)`; in `nw_draw_char` calls,
the fg argument → `pal_fg(...)`, the bg argument → `pal_bg(...)` (mind lines 72-73 where the
cursor cell swaps fg/bg — swap the wrappers accordingly). CAVEAT: verify which palette index the
VT uses for "default background" (assumed 0 here); if default is a different sentinel, key
`pal_bg` off that value instead.

- [x] **Step 3: Verify against the mockup**

Run: `make nwm-gl && make image64-gl && bash scripts/gl-desktop-show.sh`, log in, open the
Terminal, screencapture. Checklist: dark dense slab over the wallpaper (windows behind blur
through); prompt/output text solid and legible; the veil dark enough that white text reads over a
bright wallpaper; cursor cell visible; glass balls grey when the terminal loses focus. Also
`make run64` (CPU path): terminal looks exactly as today.

- [x] **Step 4: Gate + commit**

Run: `bash scripts/smoke-virtio-gpu-gl.sh`
Expected: PASS.

```bash
git add user/terminal/terminal.c
git commit -m "terminal: dark glass-client window (alpha ink over the slab)"
```

---

### Task 7: Docs + plan status

**Files:**
- Modify: `docs/en/graphics.md`, `docs/pl/graphics.md` (GL compositor section: the whole-window
  liquid-glass slab, the two ink contracts — key-black band ink vs `NW_STYLE_GLASS_CLIENT` alpha
  ink — the style word, sphere captions mapped onto the classic hit slots, `NWM_GLASS_DEBUG`,
  `NWM_NO_GL`/fallback semantics)
- Modify: this plan (check the boxes, update the STATUS line)

- [x] **Step 1: Write the docs** (mirror the header comment of `nw_compose_gl.c`; PL file matches
  EN content).
- [x] **Step 2: Commit**

```bash
git add docs/en/graphics.md docs/pl/graphics.md docs/superpowers/plans/2026-07-03-aero-liquid-glass-frames.md
git commit -m "docs: GL liquid-glass windows (slab material, ink contracts, style word, debug modes)"
```

---

## Known tradeoffs (accepted, do not "fix" en route)

- Band ink is keyed on luminance (max channel ≥ ~0.10): near-black band ink is impossible; the
  chosen title colours clear the threshold, and AA fringes toward black slightly thin the glow
  (same tradeoff as the black-keyed chrome overlay). The structural fix, if ever wanted, is
  CPU-side alpha for the band too — not a different key.
- Light-glass CLIENT apps (the mockup's Notepad/Files look) need alpha-aware painting in
  libnwui — that is a FOLLOW-ON plan, not this one. This plan delivers the material, the contract
  (`NW_STYLE_GLASS_CLIENT`), and one showcase (Terminal); legacy apps keep an opaque client on the
  slab and lose nothing.
- `glReadPixels` stays unreliable over virgl/ANGLE — never an oracle; screencapture only.
- Per-glass-window grab+blur is full-res ×2 iterations (existing cost model, pad adds ~7% pixels).
  If profiling ever demands it, the knob is a half-res blur FBO — out of scope here.
