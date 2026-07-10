/*
 * nwui_png.h — a tiny self-contained PNG decoder for the toolkit (no libpng/zlib).
 *
 * Decodes a non-interlaced 8-bit PNG (greyscale / RGB / RGBA / palette) from a memory buffer to a
 * flat 32bpp 0x00RRGGBB surface (the nw_surface pixel format), so nanowm can load the branded
 * wallpaper.png at runtime and apps can load icon PNGs. Returns a malloc'd pixel buffer (caller
 * frees) and the dimensions, or NULL on any error.
 */
#ifndef NWUI_PNG_H
#define NWUI_PNG_H

#include <stdint.h>

/* Decode `len` bytes of PNG at `data`. On success returns a malloc'd w*h array of 0x00RRGGBB
 * pixels and writes the dimensions; returns 0 on failure (unsupported format, corrupt data, OOM). */
uint32_t *png_decode(const uint8_t *data, unsigned len, int *w, int *h);

/* Read a PNG file from disk and decode it: malloc'd w*h 0x00RRGGBB array + dimensions, 0 on any
 * failure (missing file, unsupported/corrupt PNG, OOM). Caller frees. */
uint32_t *nwui_image_load_png(const char *path, int *w, int *h);

/* Why the last png_decode() returned 0 — a short static string ("chunk crc (corrupt read)",
 * "inflate", ...), "" if it succeeded. For callers' fallback diagnostics; never NULL. */
const char *png_last_error(void);

#endif /* NWUI_PNG_H */
