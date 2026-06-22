/*
 * nwset.c — NanoOS "Settings" app (libnwui). An Appearance panel that edits the desktop
 * preferences in /disks/main/nanos/config/settings.yaml: two toggles (backdrop blur, glass
 * transparency) and two 0..100 levels (blur strength, transparency level). Every change is
 * persisted to the YAML file and applied live by asking the compositor to reload it
 * (nwui_reload_settings -> NW_REQ_RELOAD_SETTINGS).
 */
#include "nwui.h"
#include "nw_settings.h"
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

static nwui          *g_u;
static struct nw_settings g_set;
static nwui_node     *g_blur_btn, *g_trans_btn;     /* On/Off toggle buttons     */
static nwui_node     *g_blurlvl_lbl, *g_translvl_lbl;/* numeric level value labels */

static void m_close(nwui_node *self, void *u) { (void) self; (void) u; _exit(0); }

/* Load settings.yaml into g_set (defaults if the file is missing/garbled). */
static void load_settings(void)
{
	nw_settings_defaults(&g_set);
	int fd = open(NW_SETTINGS_PATH, O_RDONLY);
	if (fd >= 0) {
		char buf[1024];
		int n = (int) read(fd, buf, sizeof buf - 1);
		close(fd);
		if (n > 0) nw_settings_parse(buf, n, &g_set);
	}
}

/* Persist g_set to the YAML file and ask the compositor to apply it live. */
static void save_and_apply(void)
{
	char buf[256];
	int n = nw_settings_serialize(&g_set, buf, sizeof buf);
	if (n > 0) {
		int fd = open(NW_SETTINGS_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) { write(fd, buf, n); close(fd); }
	}
	nwui_reload_settings(g_u);
}

static void refresh_labels(void)
{
	char b[8];
	nwui_set_text(g_blur_btn,  g_set.blur ? "On" : "Off");
	nwui_set_text(g_trans_btn, g_set.transparency ? "On" : "Off");
	snprintf(b, sizeof b, "%d", g_set.blur_level);          nwui_set_text(g_blurlvl_lbl, b);
	snprintf(b, sizeof b, "%d", g_set.transparency_level);  nwui_set_text(g_translvl_lbl, b);
}

static int clampL(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }

static void cb_blur(nwui_node *s, void *u)  { (void) s; (void) u; g_set.blur = !g_set.blur; refresh_labels(); save_and_apply(); }
static void cb_trans(nwui_node *s, void *u) { (void) s; (void) u; g_set.transparency = !g_set.transparency; refresh_labels(); save_and_apply(); }
static void cb_blur_dn(nwui_node *s, void *u)  { (void) s; (void) u; g_set.blur_level = clampL(g_set.blur_level - 10); refresh_labels(); save_and_apply(); }
static void cb_blur_up(nwui_node *s, void *u)  { (void) s; (void) u; g_set.blur_level = clampL(g_set.blur_level + 10); refresh_labels(); save_and_apply(); }
static void cb_trans_dn(nwui_node *s, void *u) { (void) s; (void) u; g_set.transparency_level = clampL(g_set.transparency_level - 10); refresh_labels(); save_and_apply(); }
static void cb_trans_up(nwui_node *s, void *u) { (void) s; (void) u; g_set.transparency_level = clampL(g_set.transparency_level + 10); refresh_labels(); save_and_apply(); }

static nwui_node *nav(nwui *u, const char *text, int sel)
{
	nwui_node *l = nwui_label(u, text);
	if (sel) nwui_colors(l, 0x075da0, 0);
	return nwui_pad(l, 4);
}

/* "Label . . . . <control>" — the key grows (flex) so the control sits at the right. */
static nwui_node *ctrl_row(nwui *u, const char *key, nwui_node *control)
{
	return nwui_gap(nwui_add(nwui_add(nwui_hbox(u),
	                                  nwui_flex(nwui_label(u, key), 1)),
	                         control), 8);
}

/* "[-]  value  [+]" stepper; returns the row and hands back the value label via *out_lbl. */
static nwui_node *stepper(nwui *u, nwui_cb dn, nwui_cb up, nwui_node **out_lbl)
{
	nwui_node *row = nwui_gap(nwui_hbox(u), 6);
	nwui_node *val = nwui_label(u, "0");
	nwui_size(val, 28, 0);
	*out_lbl = val;
	nwui_add(row, nwui_button(u, "-", dn, 0));
	nwui_add(row, val);
	nwui_add(row, nwui_button(u, "+", up, 0));
	return row;
}

int main(void)
{
	load_settings();
	nwui *u = nwui_open("Settings", 470, 340);
	if (!u)
		return 1;
	g_u = u;
	int ms = nwui_menu(u, "Settings"); nwui_menu_item(u, ms, "Close", m_close, 0);

	nwui_node *side = nwui_gap(nwui_pad(nwui_vbox(u), 12), 6);
	nwui_add(side, nav(u, "Appearance", 1));
	nwui_add(side, nav(u, "Network", 0));
	nwui_add(side, nav(u, "Display", 0));
	nwui_add(side, nav(u, "Privacy", 0));
	nwui_add(side, nav(u, "Accounts", 0));
	nwui_colors(side, 0, 0x00eef3f9); nwui_size(side, 132, 0);

	g_blur_btn  = nwui_button(u, "Off", cb_blur, 0);
	g_trans_btn = nwui_button(u, "Off", cb_trans, 0);
	nwui_node *blur_step  = stepper(u, cb_blur_dn,  cb_blur_up,  &g_blurlvl_lbl);
	nwui_node *trans_step = stepper(u, cb_trans_dn, cb_trans_up, &g_translvl_lbl);

	nwui_node *card = nwui_gap(nwui_pad(nwui_vbox(u), 14), 11);
	nwui_add(card, ctrl_row(u, "Backdrop blur",      g_blur_btn));
	nwui_add(card, ctrl_row(u, "Blur strength",      blur_step));
	nwui_add(card, ctrl_row(u, "Transparency",       g_trans_btn));
	nwui_add(card, ctrl_row(u, "Transparency level", trans_step));
	nwui_colors(card, 0, 0x00ffffff);

	nwui_node *main_col = nwui_gap(nwui_pad(nwui_vbox(u), 18), 10);
	nwui_add(main_col, nwui_label(u, "NanoOS"));
	nwui_add(main_col, nwui_colors(nwui_label(u, "Appearance"), 0x657184, 0));
	nwui_add(main_col, card);

	nwui_node *root = nwui_hbox(u);
	nwui_add(root, side);
	nwui_add(root, nwui_flex(main_col, 1));
	nwui_set_root(u, root);

	refresh_labels();          /* reflect the loaded settings on the controls */
	nwui_run(u);
	return 0;
}
