/*
 * nwprops.c — the system "Properties" / Get-Info window for NanOS. A STANDALONE toolkit app (its
 * own real window, managed by the window manager), NOT a modal painted inside whatever launched it.
 * Launched with a path as argv[1] — by the file manager's right-click "Properties", or from a
 * terminal (`nwprops /some/file`), exactly like `open`. It shows Name / Kind / Where / Size (files)
 * or Items (folders), and for a regular data file the "Opens with" default program with two
 * macOS-Get-Info-style controls:
 *   - "Change..."        sets the per-FILE default (the system store $HOME/.nanos-open),
 *   - "Change for type"  sets the per-EXTENSION default (associations.conf).
 * Both stores are the system-wide ones honoured by every open path (libnwui's nwui_open_file and
 * the `open` command), so the choice sticks no matter how the file is later opened.
 */
#include "nwui.h"
#include "nwui_fs.h"
#include "open/launch.h"
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>

static nwui *g_u;
static char g_path[512];
static char g_ext[16];
static int  g_isdir, g_isnxe;
static nwui_node *g_opener_label;
static int  g_picker_mode;            /* 0 = per-file, 1 = per-type */

static void m_close(nwui_node *s, void *u) { (void) s; (void) u; _exit(0); }

static const char *base_name(const char *p)
{
	const char *b = p;
	for (const char *q = p; *q; q++) if (*q == '/') b = q + 1;
	return b;
}
static void where_of(const char *path, char *out, int cap)
{
	int n = (int) strlen(path), i = n;
	while (i > 0 && path[i - 1] != '/') i--;       /* i = after the last slash */
	if (i <= 1) { out[0] = '/'; out[1] = 0; return; }   /* parent is the root */
	int k = 0;
	for (; k < i - 1 && k < cap - 1; k++) out[k] = path[k];
	out[k] = 0;
}
static const char *kind_of(void)
{
	if (g_isdir) return "Folder";
	if (g_isnxe) return "Program";
	if (!strcmp(g_ext, "png")) return "PNG image";
	static const char *const T[] = { "txt","c","h","md","cfg","conf","rs","sh","log","yaml","ini","json" };
	for (int i = 0; i < (int) (sizeof T / sizeof T[0]); i++)
		if (!strcmp(g_ext, T[i])) return "Text file";
	return "File";
}
static int dir_items(const char *path)
{
	void *d = nwui_dir_open(path);
	if (!d) return 0;
	char nm[256]; int isd, n = 0;
	while (nwui_dir_next(d, nm, sizeof nm, &isd) == 1) n++;
	nwui_dir_close(d);
	return n;
}

/* The current opener for the file: per-file override, else per-ext assoc, else "(none)". */
static void cur_opener(char *out, int cap)
{
	if (nw_file_app_lookup(g_path, out, cap)) return;
	if (nw_assoc_lookup(g_ext, out, cap)) return;
	int i = 0; for (const char *s = "(none)"; *s && i < cap - 1; s++) out[i++] = *s; out[i] = 0;
}

/* ---- writing the system default-program stores ---- */
static int read_all(const char *path, char *buf, int cap)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return 0;
	int n = (int) read(fd, buf, cap - 1);
	close(fd);
	if (n < 0) n = 0;
	buf[n] = 0;
	return n;
}
/* Upsert "path\tapp" into $HOME/.nanos-open (per-FILE default). */
static void nw_file_app_set(const char *path, const char *app)
{
	char fp[256]; nw_fileapps_path(fp, sizeof fp);
	static char in[8192], out[8192];
	read_all(fp, in, sizeof in);
	int o = 0, replaced = 0, pl = (int) strlen(path);
	for (char *line = in; line && *line; ) {
		char *nl = strchr(line, '\n');
		int len = nl ? (int) (nl - line) : (int) strlen(line);
		char *tab = (char *) memchr(line, '\t', len);
		if (tab && (int) (tab - line) == pl && !memcmp(line, path, pl)) {
			o += snprintf(out + o, sizeof out - o, "%s\t%s\n", path, app);
			replaced = 1;
		} else if (len > 0) {
			memcpy(out + o, line, len); o += len; out[o++] = '\n';
		}
		line = nl ? nl + 1 : 0;
	}
	if (!replaced) o += snprintf(out + o, sizeof out - o, "%s\t%s\n", path, app);
	int fd = open(fp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) { write(fd, out, o); close(fd); }
}
/* Upsert "ext: app" into associations.conf (per-EXTENSION default), preserving other lines. */
static void nw_type_app_set(const char *ext, const char *app)
{
	static char in[4096], out[4096];
	read_all(NW_ASSOC_PATH, in, sizeof in);
	int o = 0, replaced = 0;
	for (char *line = in; line && *line; ) {
		char *nl = strchr(line, '\n');
		int len = nl ? (int) (nl - line) : (int) strlen(line);
		int klen = 0;
		while (klen < len && line[klen] != ':' && line[klen] != '=' && line[klen] != ' ' && line[klen] != '\t') klen++;
		if (klen == (int) strlen(ext) && !memcmp(line, ext, klen)) {
			o += snprintf(out + o, sizeof out - o, "%s: %s\n", ext, app);
			replaced = 1;
		} else if (len > 0) {
			memcpy(out + o, line, len); o += len; out[o++] = '\n';
		}
		line = nl ? nl + 1 : 0;
	}
	if (!replaced) o += snprintf(out + o, sizeof out - o, "%s: %s\n", ext, app);
	int fd = open(NW_ASSOC_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) { write(fd, out, o); close(fd); }
}

static void refresh_opener_label(void)
{
	if (!g_opener_label) return;
	char op[64], buf[128];
	cur_opener(op, sizeof op);
	snprintf(buf, sizeof buf, "Opens with:  %s", op);
	nwui_set_text(g_opener_label, buf);
}
static void set_opener(const char *app)
{
	if (g_picker_mode == 0) nw_file_app_set(g_path, app);
	else if (g_ext[0])      nw_type_app_set(g_ext, app);
	nwui_close_modal(g_u);
	refresh_opener_label();
}
static void cb_pick_nwnote(nwui_node *s, void *u) { (void) s; (void) u; set_opener("nwnote"); }
static void cb_pick_nwview(nwui_node *s, void *u) { (void) s; (void) u; set_opener("nwview"); }
static void cb_pick_cancel(nwui_node *s, void *u) { (void) s; (void) u; nwui_close_modal(g_u); }

static void open_picker(int mode)
{
	g_picker_mode = mode;
	nwui_node *col = nwui_gap(nwui_pad(nwui_vbox(g_u), 14), 8);
	nwui_add(col, nwui_colors(nwui_label(g_u, mode ? "Open this type with:" : "Open this file with:"), 0x172130, 0));
	nwui_add(col, nwui_button(g_u, "Text Editor", cb_pick_nwnote, 0));
	nwui_add(col, nwui_button(g_u, "Image Viewer", cb_pick_nwview, 0));
	nwui_add(col, nwui_button(g_u, "Cancel", cb_pick_cancel, 0));
	nwui_colors(col, 0, 0x00ffffff);
	nwui_open_modal(g_u, col, 0, 0);
}
static void cb_change_file(nwui_node *s, void *u) { (void) s; (void) u; open_picker(0); }
static void cb_change_type(nwui_node *s, void *u) { (void) s; (void) u; open_picker(1); }

int main(int argc, char **argv)
{
	if (argc < 2 || !argv[1] || !argv[1][0]) return 1;
	strncpy(g_path, argv[1], sizeof g_path - 1);
	g_isdir = nwui_fs_isdir(g_path);
	int l = (int) strlen(g_path);
	g_isnxe = (l > 4 && !strcmp(g_path + l - 4, ".nxe"));
	nw_file_ext(g_path, g_ext, sizeof g_ext);

	g_u = nwui_open("Properties", 380, 280);
	if (!g_u) return 1;
	nwui_accel(g_u, 1, 'w', 0, m_close, 0);   /* Cmd+W closes */

	nwui_node *col = nwui_gap(nwui_pad(nwui_vbox(g_u), 16), 8);
	nwui_add(col, nwui_colors(nwui_label(g_u, "Properties"), 0x172130, 0));
	char buf[320];
	snprintf(buf, sizeof buf, "Name:  %s", base_name(g_path));   nwui_add(col, nwui_label(g_u, buf));
	snprintf(buf, sizeof buf, "Kind:  %s", kind_of());           nwui_add(col, nwui_label(g_u, buf));
	char where[256]; where_of(g_path, where, sizeof where);
	snprintf(buf, sizeof buf, "Where: %s", where);               nwui_add(col, nwui_label(g_u, buf));
	if (g_isdir) snprintf(buf, sizeof buf, "Items: %d", dir_items(g_path));
	else         snprintf(buf, sizeof buf, "Size:  %ld B", nwui_fs_size(g_path));
	nwui_add(col, nwui_label(g_u, buf));

	/* "Opens with" is only meaningful for a regular data file (not a folder, not a program). */
	if (!g_isdir && !g_isnxe) {
		char op[64];
		cur_opener(op, sizeof op);
		snprintf(buf, sizeof buf, "Opens with:  %s", op);
		g_opener_label = nwui_label(g_u, buf);
		nwui_add(col, g_opener_label);
		nwui_node *row = nwui_gap(nwui_hbox(g_u), 8);
		nwui_add(row, nwui_button(g_u, "Change...", cb_change_file, 0));
		nwui_add(row, nwui_button(g_u, "Change for type", cb_change_type, 0));
		nwui_add(col, row);
	}
	nwui_add(col, nwui_button(g_u, "Close", m_close, 0));
	nwui_set_root(g_u, col);
	nwui_run(g_u);
	return 0;
}
