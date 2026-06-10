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
 * drawn separately as a cheap overlay so plain mouse motion never repaints windows. */
void nw_compose_scene(const struct nw_server *s, const struct nw_surface *back);

/* Draw the arrow cursor at (x,y) onto `dst` (e.g. directly onto the framebuffer surface). */
void nw_draw_cursor(const struct nw_surface *dst, int x, int y);

/* Scene + cursor in one call (used by host tests / the simple path). */
void nw_compose(const struct nw_server *s, const struct nw_surface *back);

#endif /* NW_COMPOSE_H */
