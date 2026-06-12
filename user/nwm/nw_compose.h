/*
 * nw_compose.h — pure compositing: render the compositor's window list (z-order,
 * decorations, focus highlight, content, mouse cursor) into a backbuffer surface. No I/O;
 * the I/O shell then blits the backbuffer to /dev/fb0. Host-tested by composing a server
 * state into an in-memory surface and asserting pixels.
 */
#ifndef NW_COMPOSE_H
#define NW_COMPOSE_H

#include "nwm_core.h"
#include "nw_gfx.h"

/* Arrow cursor extent (for damage/overlay math in the I/O shell). */
enum { NW_CURSOR_W = 11, NW_CURSOR_H = 16 };

/* Paint the desktop + every window back-to-front into `back`, WITHOUT the cursor. The shell
 * caches this "scene" and only recomposes it when the scene actually changes; the cursor is
 * drawn separately as a cheap overlay so plain mouse motion never repaints windows.
 *
 * `wall` is the pre-rendered gradient wallpaper (blitted as the base; NULL = flat fill).
 * `scratch` is a screen-sized work buffer used to render each window opaquely before it is
 * composited onto the scene with rounded corners + translucency (NULL = opaque square path,
 * used by simple/host paths). Both are caller-owned. */
void nw_compose_scene(const struct nw_server *s, const struct nw_surface *back,
                      const struct nw_surface *scratch, const struct nw_surface *wall);

/* Re-render every window whose cached frame is marked dirty (content/focus/create changed) into
 * its window-local frame buffer, clearing the flag. Run this before nw_compose_scene: windows
 * with a frame buffer are then composited from the cache, so a drag (which only moves x/y, never
 * dirtying) costs no chrome/content re-render. Windows without a frame fall back to the live path. */
void nw_render_dirty_frames(struct nw_server *s);

/* Render the static gradient wallpaper into `dst` once (the shell caches it). */
void nw_render_wallpaper(const struct nw_surface *dst);

/* Draw the arrow cursor at (x,y) onto `dst` (e.g. directly onto the framebuffer surface). */
void nw_draw_cursor(const struct nw_surface *dst, int x, int y);

/* Scene + cursor in one call (used by host tests / the simple path). */
void nw_compose(const struct nw_server *s, const struct nw_surface *back);

#endif /* NW_COMPOSE_H */
