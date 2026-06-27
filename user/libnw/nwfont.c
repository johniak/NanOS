/* nwfont.c — TTF/OTF font engine over stb_truetype (see nwfont.h). Per-role face with a lazy
 * 256-entry (ASCII + Latin-1) AA glyph cache. The font file buffer is kept alive for the face's
 * lifetime (stb_truetype references it). */
#include "nwfont.h"
#include "stb_truetype.h"      /* declarations only; the implementation lives in stb_impl.c */
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#define NCACHE 256

struct face {
    int            loaded;
    unsigned char *file;       /* kept alive for stb_truetype */
    stbtt_fontinfo info;
    float          scale;
    int            ascent, line_h;
    struct nwfont_glyph g[NCACHE];
    unsigned char *bm[NCACHE];
    int            have[NCACHE];
};
static struct face faces[2];

static unsigned char *read_file(const char *path, int cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    unsigned char *buf = (unsigned char *) malloc(cap);
    if (!buf) { close(fd); return 0; }
    int total = 0, n;
    while (total < cap && (n = (int) read(fd, buf + total, cap - total)) > 0) total += n;
    close(fd);
    if (total <= 0) { free(buf); return 0; }
    return buf;
}

int nwfont_set(int role, const char *path, int px)
{
    if (role < 0 || role > 1 || px <= 0) return -1;
    struct face *f = &faces[role];
    unsigned char *buf = read_file(path, 4 * 1024 * 1024);
    if (!buf) return -1;
    int off = stbtt_GetFontOffsetForIndex(buf, 0);
    stbtt_fontinfo info;
    if (off < 0 || !stbtt_InitFont(&info, buf, off)) { free(buf); return -1; }

    if (f->loaded) {                       /* drop the previous face + cache */
        for (int i = 0; i < NCACHE; i++) {
            if (f->bm[i]) { stbtt_FreeBitmap(f->bm[i], 0); f->bm[i] = 0; }
            f->have[i] = 0;
        }
        free(f->file);
    } else {
        for (int i = 0; i < NCACHE; i++) { f->bm[i] = 0; f->have[i] = 0; }
    }

    f->file  = buf;
    f->info  = info;
    f->scale = stbtt_ScaleForPixelHeight(&info, (float) px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    f->ascent = (int) (asc * f->scale + 0.5f);
    f->line_h = (int) ((asc - desc + gap) * f->scale + 0.5f);
    if (f->line_h < px) f->line_h = px;
    f->loaded = 1;
    return 0;
}

int nwfont_loaded(int role) { return (role >= 0 && role < 2) ? faces[role].loaded : 0; }
int nwfont_ascent(int role) { return (role >= 0 && role < 2) ? faces[role].ascent : 0; }
int nwfont_line_h(int role) { return (role >= 0 && role < 2) ? faces[role].line_h : 0; }

const struct nwfont_glyph *nwfont_get(int role, unsigned cp)
{
    if (role < 0 || role > 1 || !faces[role].loaded || cp >= NCACHE) return 0;
    struct face *f = &faces[role];
    if (!f->have[cp]) {
        int aw, lsb;
        stbtt_GetCodepointHMetrics(&f->info, (int) cp, &aw, &lsb);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&f->info, (int) cp, f->scale, f->scale, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0;
        unsigned char *bm = 0;
        if (w > 0 && h > 0)
            bm = stbtt_GetCodepointBitmap(&f->info, f->scale, f->scale, (int) cp, &w, &h, 0, 0);
        f->bm[cp]      = bm;
        f->g[cp].cov   = bm;
        f->g[cp].w     = bm ? w : 0;
        f->g[cp].h     = bm ? h : 0;
        f->g[cp].advance = (int) (aw * f->scale + 0.5f);
        f->g[cp].bx    = x0;
        f->g[cp].top   = y0;     /* stb bitmap-box: y down, y0 = top relative to baseline */
        f->have[cp]    = 1;
    }
    return &f->g[cp];
}

int nwfont_text_w(int role, const char *s)
{
    if (!s || !nwfont_loaded(role)) return 0;
    int w = 0;
    for (; *s; s++) {
        const struct nwfont_glyph *g = nwfont_get(role, (unsigned char) *s);
        if (g) w += g->advance;
    }
    return w;
}
