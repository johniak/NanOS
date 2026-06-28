/*
 * nwview.c — a minimal image viewer for NanOS, built entirely from REUSABLE toolkit pieces:
 * the libnwui PNG decoder (nwui_image_load_png) + the image widget (nwui_image). It is the
 * "open with" target for images — the file manager's nwui_open_file() launches it with a PNG
 * path as argv[1] (macOS-style `open`). A deliberately tiny app, to show the image components
 * are app-agnostic and reusable.
 */
#include "nwui.h"
#include <stdint.h>
#include <unistd.h>
#include <string.h>

static nwui *g_u;
static void m_close(nwui_node *s, void *u) { (void) s; (void) u; _exit(0); }

/* The last path component, for the window/title label. */
static const char *base_name(const char *p)
{
	const char *b = p;
	for (const char *q = p; *q; q++) if (*q == '/') b = q + 1;
	return b;
}

int main(int argc, char **argv)
{
	g_u = nwui_open("Image Viewer", 680, 520);
	if (!g_u) return 1;
	int mf = nwui_menu(g_u, "Image");
	nwui_menu_item(g_u, mf, "Close", m_close, 0);
	nwui_accel(g_u, 1, 'w', 0, m_close, 0);   /* Cmd+W closes (macOS-style) */

	nwui_node *col = nwui_gap(nwui_pad(nwui_vbox(g_u), 10), 8);
	if (argc > 1 && argv[1] && argv[1][0]) {
		nwui_add(col, nwui_colors(nwui_label(g_u, base_name(argv[1])), 0x172130, 0));
		int w = 0, h = 0;
		uint32_t *px = nwui_image_load_png(argv[1], &w, &h);
		if (px && w > 0 && h > 0)
			nwui_add(col, nwui_image(g_u, px, w, h));
		else
			nwui_add(col, nwui_label(g_u, "Cannot open this image."));
	} else {
		nwui_add(col, nwui_label(g_u, "No image given."));
	}
	nwui_set_root(g_u, col);
	nwui_run(g_u);
	return 0;
}
