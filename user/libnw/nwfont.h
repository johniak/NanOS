/*
 * nwfont.h — the userland TTF/OTF font engine (over stb_truetype). Two roles: a proportional UI
 * font and a fixed monospace font (the terminal). Loads a font file, rasterizes anti-aliased
 * glyphs on demand into a per-codepoint cache, and reports metrics. nw_gfx blends the coverage.
 */
#ifndef NWFONT_H
#define NWFONT_H

enum { NWFONT_UI = 0, NWFONT_MONO = 1 };

struct nwfont_glyph {
    const unsigned char *cov;   /* w*h coverage bytes (0..255), or 0 for a blank glyph (e.g. space) */
    int w, h;                   /* coverage bitmap size in pixels */
    int advance;                /* pen advance after this glyph (px) */
    int bx;                     /* left bearing: x offset of the bitmap from the pen (px) */
    int top;                    /* top of the bitmap relative to the baseline (px; negative = above) */
};

/* Load/replace a role from a TTF/OTF file, rendered at `px` pixel height. Returns 0 on success,
 * <0 on failure (missing file, bad font, OOM); on failure the role stays unloaded. */
int nwfont_set(int role, const char *path, int px);
int nwfont_loaded(int role);
int nwfont_ascent(int role);                       /* baseline offset from the line-box top (px) */
int nwfont_line_h(int role);                       /* line height (px) */
int nwfont_text_w(int role, const char *s);        /* total advance width of a NUL-terminated string */
const struct nwfont_glyph *nwfont_get(int role, unsigned cp);   /* cached glyph, or 0 */

#endif /* NWFONT_H */
