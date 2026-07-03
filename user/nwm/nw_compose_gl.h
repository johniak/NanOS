/*
 * nw_compose_gl.h — nwm's GL ES present backend (Plan 1, Task 10). It takes the scene surface the
 * mature CPU compositor already renders (nw_compose_scene → g_scene: wallpaper, glass windows with
 * blur, rounded corners, focus ring, cursor) and PRESENTS it to the physical display through the
 * canonical Linux GPU path — a GBM scanout surface + EGL ES context (glkms_init.h), uploaded as a
 * full-screen GL texture and scanned out via KMS (eglSwapBuffers → drmModeSetCrtc). No CPU readback
 * and no /dev/fb0 blit: the host GPU (virglrenderer→ANGLE→Metal on QEMU; i915 on the Dell) resolves
 * the rendered buffer to scanout. Reusing the CPU scene guarantees pixel parity (same z-order,
 * glass, blur, focus, cursor); moving per-window compositing and the Gaussian blur onto the GPU is
 * the documented follow-on that grows inside nw_gl_frame without touching nwm.c.
 *
 * All functions return 0 on success. Any failure after init disables the backend for the session:
 * nwm logs it and falls back to the CPU fb0 path (nw_gl_active() then returns 0). The kill-switch is
 * the NWM_NO_GL env var (checked by nwm.c before calling nw_gl_init) plus a plain init failure on a
 * host with no DRM node (plain QEMU) — both land on the untouched CPU path.
 */
#ifndef NW_COMPOSE_GL_H
#define NW_COMPOSE_GL_H

struct nw_surface;   /* user/libnw/nw_gfx.h — the composited scene (0x00RRGGBB pixels, tight rows) */

/* glkms_open (card0 + GBM scanout surface + EGL ES context) + compile the present program and
 * allocate the screen-sized upload texture. screen_w/h must equal the scene surface dimensions.
 * Returns 0 on success; -1 (with state cleaned up) on any failure so the caller can use the CPU
 * path. */
int  nw_gl_init(int screen_w, int screen_h);

/* Present one composed frame: upload `scene` into the screen texture, draw the full-screen quad,
 * and scan it out (glkms_swap). Returns 0 on success; -1 on any GL/KMS error — the caller then
 * shuts the backend down and falls back to CPU for the session. */
int  nw_gl_frame(const struct nw_surface *scene);

/* Release EGL/GBM/KMS + GL resources. Idempotent; safe on a never-initialised backend. */
void nw_gl_shutdown(void);

/* 1 while the GL backend is live, 0 once it has never initialised or has fallen back to CPU. */
int  nw_gl_active(void);

#endif /* NW_COMPOSE_GL_H */
