/*
 * vt.h — a pure VT/ANSI terminal engine: the screen grid + an xterm-subset escape parser, with
 * NO I/O and NO rendering. Shared by the framebuffer terminal (nterm) and the windowed terminal
 * (nwterm); each renders the grid its own way. Pure logic, host-testable.
 *
 * Handles the xterm subset shells/TUIs need: printable text, CR/LF/BS/TAB, CSI cursor moves +
 * absolute position, erase line/display, SGR colours (16 + 256 + truecolor→256), scroll region,
 * cursor save/restore. A per-row dirty flag lets the renderer repaint only changed rows.
 */
#ifndef NX_VT_H
#define NX_VT_H

#include <stdint.h>

/* C linkage even when included from C++ (the kernel's FbConsole), so the same vt.o/vtk.o links
 * against C++ and C callers alike regardless of how the .c is compiled. */
#ifdef __cplusplus
extern "C" {
#endif

enum { VT_MAXC = 256, VT_MAXR = 128, VT_NPAR = 16 };

typedef struct { unsigned char ch, fg, bg; } vt_cell;

typedef struct {
	vt_cell grid[VT_MAXR][VT_MAXC];
	int cols, rows;
	int cx, cy;                 /* cursor */
	int fg, bg, bold, rev;      /* current SGR attributes */
	int top, bot;               /* scroll region [top,bot] */
	int savecx, savecy;
	int state, par[VT_NPAR], npar, priv;   /* parser */
	unsigned char dirty[VT_MAXR];          /* row changed since last render */
	/* Alternate screen (xterm DECSET 47/1047/1049): a full-screen app (vim, less, top) switches
	 * to a blank scratch screen on entry and the main screen is restored on exit — so the shell's
	 * prompt + scrollback reappear instead of the app's leftover frame. `save` holds the main
	 * screen while `alt` is set; `alt_cx/cy` is the main-screen cursor to restore. */
	vt_cell save[VT_MAXR][VT_MAXC];
	int alt, alt_cx, alt_cy;
} vt;

void     vt_init(vt *t, int cols, int rows);          /* clear grid, cursor home */
void     vt_resize(vt *t, int cols, int rows);        /* change geometry, clamp cursor */
void     vt_feed(vt *t, const unsigned char *b, int n); /* process `n` shell-output bytes */
uint32_t vt_pal(int idx);                             /* palette index (0..255) -> 0x00RRGGBB */

#ifdef __cplusplus
}
#endif

#endif /* NX_VT_H */
