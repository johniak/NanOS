/*
 * png.h — a tiny self-contained PNG decoder for the compositor (no libpng/zlib).
 *
 * Decodes a non-interlaced 8-bit PNG (greyscale / RGB / RGBA / palette) from a memory buffer to a
 * flat 32bpp 0x00RRGGBB surface (the nw_surface pixel format), so NanWM can load the branded
 * wallpaper.png at runtime and fit it to whatever resolution the firmware gave us. Returns a
 * malloc'd pixel buffer (caller frees) and the dimensions, or NULL on any error.
 */
#ifndef NWM_PNG_H
#define NWM_PNG_H

#include <stdint.h>

/* Decode `len` bytes of PNG at `data`. On success returns a malloc'd w*h array of 0x00RRGGBB
 * pixels and writes the dimensions; returns 0 on failure (unsupported format, corrupt data, OOM). */
uint32_t *png_decode(const uint8_t *data, unsigned len, int *w, int *h);

#endif /* NWM_PNG_H */
