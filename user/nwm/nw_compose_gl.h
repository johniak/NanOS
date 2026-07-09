/*
 * nw_compose_gl.h — nwm's GPU-native GL ES compositor backend (Plan 1, Task 10). It composites the
 * desktop on the GPU: wallpaper + per-window content textures + a REAL GPU two-pass Gaussian blur of
 * each glass window's backdrop (grabbed from the framebuffer, blurred in an FBO), sampled under a
 * rounded-corner mask at the per-window body alpha. The desktop chrome (panel/taskbar/dropdowns/
 * Run+Auth modals) is rendered by the CPU 2D toolkit into a transparent overlay (nw_compose_chrome)
 * and drawn last; the cursor is a small keyed quad. The composited frame is scanned out through
 * GBM+EGL+KMS (glkms_init.h): eglSwapBuffers → drmModeSetCrtc, no CPU readback — the host GPU
 * (virglrenderer→ANGLE→Metal on QEMU; i915 on the Dell) resolves the buffer to scanout.
 *
 * All functions return 0 on success. Any failure after init disables the backend for the session:
 * nwm logs it and falls back to the CPU fb0 path (nw_gl_active() then returns 0). Kill-switch =
 * NWM_NO_GL (checked by nwm.c before nw_gl_init) plus a plain init failure with no DRM node.
 */
#ifndef NW_COMPOSE_GL_H
#define NW_COMPOSE_GL_H

struct nw_server;    /* user/nwm/nwm_core.h — window list + z-order + cursor */
struct nw_surface;   /* user/libnw/nw_gfx.h — the wallpaper (and the CPU chrome overlay) */

/* glkms_open (card0 + GBM scanout + EGL ES context) + compile the compositor programs and allocate
 * the chrome-overlay surface + texture. screen_w/h are the display size. Returns 0 on success; -1
 * (state cleaned up) on any failure so the caller can use the CPU path. */
int  nw_gl_init(int screen_w, int screen_h);

/* Present one frame. When scene_dirty is nonzero, recompose the cursor-free desktop on the GPU from
 * the server's window list (each window's cached `frame` render) over `wall` (the wallpaper), with
 * GPU glass blur and the CPU chrome overlay, into the offscreen scene; the caller must have run
 * nw_render_dirty_frames() first. When scene_dirty is 0 (a bare cursor move) the scene is reused as-is
 * — skipping the whole TCG-expensive composite. Either way the scene is blitted to the display with
 * the cursor drawn on top at its live position, then scanned out (glkms_swap).
 *
 * A move-only frame uploads ZERO texture bytes: window content re-uploads only when its frame_gen
 * changed, the wallpaper only when nw_gl_wallpaper_changed() flagged it, and when `interacting` is
 * nonzero (a drag/resize is live) the chrome overlay is neither re-rendered nor re-uploaded — the
 * panel/taskbar can't change mid-drag, so the resident chrome texture is reused as-is.
 * Returns 0 on success; -1 on any GL/KMS error (caller falls back to CPU). */
int  nw_gl_frame(const struct nw_server *s, const struct nw_surface *wall, int scene_dirty,
                 int interacting);

/* Mark the wallpaper texture stale so the next frame re-uploads it (call after a settings reload
 * repaints the wallpaper into the same buffer). Cheap; safe before init. */
void nw_gl_wallpaper_changed(void);

/* Build the cursor texture from the CPU arrow bitmap (once). Call after nw_gl_init succeeds. */
void nw_gl_build_cursor(void);

/* Set the window corner radius (0..20) used by the glass shader; mirrors nw_compose_set_theme. */
void nw_gl_set_radius(int radius);

/* Release EGL/GBM/KMS + all GL resources. Idempotent; safe on a never-initialised backend. */
void nw_gl_shutdown(void);

/* Mid-session FAILURE teardown (the GL->CPU fallback): the wedged context's fences could make a
 * graceful teardown block forever (Dell boot #49 desktop freeze). Restores the scanout + closes
 * the DRM fd only, deliberately leaking the GL state — never blocks. */
void nw_gl_shutdown_wedged(void);

/* 1 while the GL backend is live, 0 once it has never initialised or has fallen back to CPU. */
int  nw_gl_active(void);

#endif /* NW_COMPOSE_GL_H */
