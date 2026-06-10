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

/* Paint the whole desktop (background + every window back-to-front + cursor) into `back`. */
void nw_compose(const struct nw_server *s, const struct nw_surface *back);

#endif /* NW_COMPOSE_H */
