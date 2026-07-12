/*
 * settings.c — Nano OS "Settings" app (libnwui). An Appearance panel that edits the desktop
 * preferences in /disks/main/nanos/config/settings.yaml: two toggles (backdrop blur, glass
 * transparency) and two 0..100 levels (blur strength, transparency level). Every change is
 * persisted to the YAML file and applied live by asking the compositor to reload it
 * (nwui_reload_settings -> NW_REQ_RELOAD_SETTINGS).
 */
#include "nwui.h"
#include "nwui_fs.h"
#include "nwproto.h"   /* NW_STYLE_* */
#include "nw_settings.h"
#include "nw_settings_path.h"
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

static nwui          *g_u;
static struct nw_settings g_set;
static nwui_node     *g_blur_btn, *g_trans_btn;     /* On/Off toggle buttons     */
static nwui_node     *g_blurlvl_lbl, *g_translvl_lbl;/* numeric level value labels */
static nwui_node     *g_accent_btn, *g_wall_btn;    /* accent / wallpaper cycle buttons */
static nwui_node     *g_clk24_btn, *g_clksec_btn, *g_shadow_btn;  /* On/Off toggles */
static nwui_node     *g_radius_lbl;                 /* corner-radius value label */
static nwui_node     *g_font_btn;                   /* UI font cycle button */

static const unsigned ACCENTS[]     = { 0x12a8f4, 0x7d3ff2, 0x6fd033, 0xff9d00, 0xe81123, 0x657184 };
static const char *const ACC_NAMES[]= { "Blue", "Purple", "Green", "Orange", "Red", "Graphite" };
enum { NACC = 6 };
static const char *const WALL_NAMES[] = { "Branded", "Gradient", "Solid" };

static int accent_index(unsigned c) { for (int i = 0; i < NACC; i++) if (ACCENTS[i] == c) return i; return 0; }

static void m_close(nwui_node *self, void *u) { (void) self; (void) u; _exit(0); }

/* Overlay any settings in the file at `path` onto g_set (no-op if absent/empty). */
static void overlay_file(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return;
	char buf[1024];
	int n = (int) read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n > 0) nw_settings_parse(buf, n, &g_set);
}

/* Load the effective preferences into g_set: shipped defaults, then the system-wide file (if any),
 * then the per-user file in $HOME (which wins). Matches the layering nwm applies (apply_settings). */
static void load_settings(void)
{
	char up[256];
	nw_settings_defaults(&g_set);
	overlay_file(NW_SETTINGS_PATH);              /* system-wide default (optional) */
	nw_settings_user_path(up, sizeof up);
	overlay_file(up);                            /* per-user override (wins) */
}

/* Persist g_set to the PER-USER file in $HOME (the system config dir is root-owned and not writable
 * by the unprivileged desktop), then ask the compositor to apply it live. */
static void save_and_apply(void)
{
	char buf[256], up[256];
	int n = nw_settings_serialize(&g_set, buf, sizeof buf);
	if (n > 0) {
		nw_settings_user_path(up, sizeof up);
		int fd = open(up, O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
	nwui_set_text(g_accent_btn, ACC_NAMES[accent_index(g_set.accent)]);
	nwui_set_text(g_wall_btn,   WALL_NAMES[g_set.wallpaper % 3]);
	nwui_set_text(g_clk24_btn,  g_set.clock_24h ? "24h" : "12h");
	nwui_set_text(g_clksec_btn, g_set.clock_seconds ? "On" : "Off");
	nwui_set_text(g_shadow_btn, g_set.shadow ? "On" : "Off");
	snprintf(b, sizeof b, "%d", g_set.corner_radius);       nwui_set_text(g_radius_lbl, b);
	nwui_set_text(g_font_btn, g_set.ui_font[0] ? g_set.ui_font : NW_UI_FONT_DEFAULT);
}

static int clampL(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }
static int clampR(int v) { return v < 0 ? 0 : (v > 20 ? 20 : v); }

static void cb_blur(nwui_node *s, void *u)  { (void) s; (void) u; g_set.blur = !g_set.blur; refresh_labels(); save_and_apply(); }
static void cb_trans(nwui_node *s, void *u) { (void) s; (void) u; g_set.transparency = !g_set.transparency; refresh_labels(); save_and_apply(); }
static void cb_blur_dn(nwui_node *s, void *u)  { (void) s; (void) u; g_set.blur_level = clampL(g_set.blur_level - 10); refresh_labels(); save_and_apply(); }
static void cb_blur_up(nwui_node *s, void *u)  { (void) s; (void) u; g_set.blur_level = clampL(g_set.blur_level + 10); refresh_labels(); save_and_apply(); }
static void cb_trans_dn(nwui_node *s, void *u) { (void) s; (void) u; g_set.transparency_level = clampL(g_set.transparency_level - 10); refresh_labels(); save_and_apply(); }
static void cb_trans_up(nwui_node *s, void *u) { (void) s; (void) u; g_set.transparency_level = clampL(g_set.transparency_level + 10); refresh_labels(); save_and_apply(); }
static void cb_accent(nwui_node *s, void *u) { (void) s; (void) u; g_set.accent = ACCENTS[(accent_index(g_set.accent) + 1) % NACC]; refresh_labels(); save_and_apply(); }
static void cb_wall(nwui_node *s, void *u)   { (void) s; (void) u; g_set.wallpaper = (g_set.wallpaper + 1) % 3; refresh_labels(); save_and_apply(); }
static void cb_clk24(nwui_node *s, void *u)  { (void) s; (void) u; g_set.clock_24h = !g_set.clock_24h; refresh_labels(); save_and_apply(); }
static void cb_clksec(nwui_node *s, void *u) { (void) s; (void) u; g_set.clock_seconds = !g_set.clock_seconds; refresh_labels(); save_and_apply(); }
static void cb_shadow(nwui_node *s, void *u) { (void) s; (void) u; g_set.shadow = !g_set.shadow; refresh_labels(); save_and_apply(); }
static void cb_radius_dn(nwui_node *s, void *u) { (void) s; (void) u; g_set.corner_radius = clampR(g_set.corner_radius - 2); refresh_labels(); save_and_apply(); }
static void cb_radius_up(nwui_node *s, void *u) { (void) s; (void) u; g_set.corner_radius = clampR(g_set.corner_radius + 2); refresh_labels(); save_and_apply(); }
static int is_font_file(const char *n) { int l = (int) strlen(n); return l > 4 && (!strcmp(n + l - 4, ".ttf") || !strcmp(n + l - 4, ".otf")); }
/* Cycle the UI font through the .ttf/.otf files in /nanos/share/fonts. */
static void cb_font(nwui_node *s, void *u)
{
	(void) s; (void) u;
	char names[16][64];
	int cnt = 0;
	void *d = nwui_dir_open(NW_FONTS_DIR);
	if (d) {
		char nm[64]; int isd;
		while (cnt < 16 && nwui_dir_next(d, nm, sizeof nm, &isd) == 1)
			if (is_font_file(nm)) { strncpy(names[cnt], nm, 63); names[cnt][63] = 0; cnt++; }
		nwui_dir_close(d);
	}
	if (cnt == 0) return;
	int cur = 0;
	for (int i = 0; i < cnt; i++) if (!strcmp(names[i], g_set.ui_font)) cur = i;
	int nx = (cur + 1) % cnt;
	strncpy(g_set.ui_font, names[nx], 63); g_set.ui_font[63] = 0;
	refresh_labels(); save_and_apply();
}

/* ---- file associations ("Default Apps" category): edit /nanos/config/associations.conf ---- */
#define ASSOC_PATH "/disks/main/nanos/config/associations.conf"
static const char *const TEXT_EXTS[] = { "txt","c","h","md","cfg","conf","rs","sh","log","yaml","ini","json" };
static const char *const IMG_EXTS[]  = { "png" };
static const char *const APP_CHOICES[] = { "notepad", "viewer" };
enum { NTEXT_EXTS = 12, NIMG_EXTS = 1, NAPP_CHOICES = 2 };
static char g_text_app[32] = "notepad";   /* app that opens text files  */
static char g_img_app[32]  = "viewer";   /* app that opens images      */
static nwui_node *g_textapp_btn, *g_imgapp_btn;

static void assoc_load(void)
{
	char a[32];
	if (nwui_assoc_lookup("txt", a, sizeof a)) { strncpy(g_text_app, a, 31); g_text_app[31] = 0; }
	if (nwui_assoc_lookup("png", a, sizeof a)) { strncpy(g_img_app,  a, 31); g_img_app[31]  = 0; }
}
static void assoc_save(void)
{
	char buf[1024];
	int n = snprintf(buf, sizeof buf, "# file associations (ext: app) - edited by Settings\n");
	for (int i = 0; i < NTEXT_EXTS && n < (int) sizeof buf - 64; i++)
		n += snprintf(buf + n, sizeof buf - n, "%s: %s\n", TEXT_EXTS[i], g_text_app);
	for (int i = 0; i < NIMG_EXTS && n < (int) sizeof buf - 64; i++)
		n += snprintf(buf + n, sizeof buf - n, "%s: %s\n", IMG_EXTS[i], g_img_app);
	int fd = open(ASSOC_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) { write(fd, buf, n); close(fd); }
}
static const char *cycle_app(const char *cur)
{
	for (int i = 0; i < NAPP_CHOICES; i++)
		if (!strcmp(cur, APP_CHOICES[i])) return APP_CHOICES[(i + 1) % NAPP_CHOICES];
	return APP_CHOICES[0];
}
static void refresh_apps(void)
{
	nwui_set_text(g_textapp_btn, g_text_app);
	nwui_set_text(g_imgapp_btn,  g_img_app);
}
static void cb_textapp(nwui_node *s, void *u) { (void) s; (void) u; strncpy(g_text_app, cycle_app(g_text_app), 31); g_text_app[31] = 0; refresh_apps(); assoc_save(); }
static void cb_imgapp(nwui_node *s, void *u)  { (void) s; (void) u; strncpy(g_img_app,  cycle_app(g_img_app),  31); g_img_app[31]  = 0; refresh_apps(); assoc_save(); }

/* ---- category switching (Appearance / Desktop / Default Apps) ---- */
enum { CAT_APPEARANCE, CAT_DESKTOP, CAT_APPS, NCAT };
static const char *const CAT_NAME[NCAT] = { "Appearance", "Desktop", "Default Apps" };
static nwui_node *g_panel[NCAT];     /* per-category content cards (only one visible) */
static nwui_node *g_nav[NCAT];       /* sidebar nav rows */
static nwui_node *g_title;           /* big title above the active panel */

static void show_cat(int c)
{
	for (int i = 0; i < NCAT; i++) {
		nwui_set_visible(g_panel[i], i == c);
		nwui_link_set_active(g_nav[i], i == c);
	}
	nwui_set_text(g_title, CAT_NAME[c]);
}
static void cb_cat0(nwui_node *s, void *u) { (void) s; (void) u; show_cat(CAT_APPEARANCE); }
static void cb_cat1(nwui_node *s, void *u) { (void) s; (void) u; show_cat(CAT_DESKTOP); }
static void cb_cat2(nwui_node *s, void *u) { (void) s; (void) u; show_cat(CAT_APPS); }

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
	assoc_load();
	nwui *u = nwui_open_style("Settings", 520, 480, NW_STYLE_GLASS_CLIENT);
	if (!u)
		return 1;
	g_u = u;
	int ms = nwui_menu(u, "Settings"); nwui_menu_item(u, ms, "Close", m_close, 0);

	/* sidebar: one clickable nav row per category (the active one renders as an accent pill) */
	nwui_node *side = nwui_gap(nwui_pad(nwui_vbox(u), 12), 4);
	nwui_cb navcb[NCAT] = { cb_cat0, cb_cat1, cb_cat2 };
	for (int i = 0; i < NCAT; i++) {
		g_nav[i] = nwui_link(u, CAT_NAME[i], navcb[i], 0);
		nwui_add(side, g_nav[i]);
	}
	nwui_colors(side, 0, 0x00eef3f9); nwui_size(side, 140, 0);

	/* ---- Appearance panel (visual look of the desktop) ---- */
	g_blur_btn   = nwui_button(u, "Off", cb_blur, 0);
	g_trans_btn  = nwui_button(u, "Off", cb_trans, 0);
	g_accent_btn = nwui_button(u, "Blue", cb_accent, 0);
	g_wall_btn   = nwui_button(u, "Branded", cb_wall, 0);
	g_shadow_btn = nwui_button(u, "On", cb_shadow, 0);
	g_font_btn   = nwui_button(u, "UISans-Regular.ttf", cb_font, 0);
	nwui_node *blur_step   = stepper(u, cb_blur_dn,   cb_blur_up,   &g_blurlvl_lbl);
	nwui_node *trans_step  = stepper(u, cb_trans_dn,  cb_trans_up,  &g_translvl_lbl);
	nwui_node *radius_step = stepper(u, cb_radius_dn, cb_radius_up, &g_radius_lbl);
	nwui_node *appear = nwui_gap(nwui_pad(nwui_vbox(u), 14), 9);
	nwui_add(appear, ctrl_row(u, "Backdrop blur",      g_blur_btn));
	nwui_add(appear, ctrl_row(u, "Blur strength",      blur_step));
	nwui_add(appear, ctrl_row(u, "Transparency",       g_trans_btn));
	nwui_add(appear, ctrl_row(u, "Transparency level", trans_step));
	nwui_add(appear, ctrl_row(u, "Accent colour",      g_accent_btn));
	nwui_add(appear, ctrl_row(u, "Wallpaper",          g_wall_btn));
	nwui_add(appear, ctrl_row(u, "Window shadow",      g_shadow_btn));
	nwui_add(appear, ctrl_row(u, "Corner radius",      radius_step));
	nwui_add(appear, ctrl_row(u, "UI font",            g_font_btn));
	nwui_colors(appear, 0, 0x00ffffff);
	g_panel[CAT_APPEARANCE] = appear;

	/* ---- Desktop panel (clock + menu bar — moved out of Appearance) ---- */
	g_clk24_btn  = nwui_button(u, "24h", cb_clk24, 0);
	g_clksec_btn = nwui_button(u, "Off", cb_clksec, 0);
	nwui_node *desktop = nwui_gap(nwui_pad(nwui_vbox(u), 14), 9);
	nwui_add(desktop, ctrl_row(u, "Clock format",  g_clk24_btn));
	nwui_add(desktop, ctrl_row(u, "Clock seconds", g_clksec_btn));
	nwui_colors(desktop, 0, 0x00ffffff);
	g_panel[CAT_DESKTOP] = desktop;

	/* ---- Default Apps panel (file associations -> nwui_open_file / `open`) ---- */
	g_textapp_btn = nwui_button(u, g_text_app, cb_textapp, 0);
	g_imgapp_btn  = nwui_button(u, g_img_app,  cb_imgapp, 0);
	nwui_node *apps = nwui_gap(nwui_pad(nwui_vbox(u), 14), 9);
	nwui_add(apps, ctrl_row(u, "Text files (.txt .md .c .sh …)", g_textapp_btn));
	nwui_add(apps, ctrl_row(u, "Images (.png)",                  g_imgapp_btn));
	nwui_add(apps, nwui_colors(nwui_label(u, "Opens with — used by the file manager's Open."), 0x8a8a8e, 0));
	nwui_colors(apps, 0, 0x00ffffff);
	g_panel[CAT_APPS] = apps;

	g_title = nwui_colors(nwui_label(u, CAT_NAME[CAT_APPEARANCE]), 0x657184, 0);
	nwui_node *main_col = nwui_gap(nwui_pad(nwui_vbox(u), 18), 10);
	nwui_add(main_col, nwui_label(u, "Nano OS"));
	nwui_add(main_col, g_title);
	for (int i = 0; i < NCAT; i++)
		nwui_add(main_col, g_panel[i]);

	nwui_node *root = nwui_hbox(u);
	nwui_add(root, side);
	nwui_add(root, nwui_flex(main_col, 1));
	nwui_set_root(u, root);

	refresh_labels();          /* reflect the loaded settings on the controls */
	refresh_apps();
	show_cat(CAT_APPEARANCE);  /* start on Appearance; hides the other panels */
	nwui_run(u);
	return 0;
}
