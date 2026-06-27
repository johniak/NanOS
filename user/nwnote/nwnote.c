/*
 * nwnote.c — NanOS Notepad: a Windows XP-style text editor built on libnwui. The editor itself
 * is the reusable nwui_textarea widget; this thin client only wires the File/Edit/Format/View/
 * Help menus + matching keyboard accelerators, the status bar (Ln/Col + filename), file open/
 * save via libc, find / find-next / replace / go-to dialogs, single-level undo, and the About
 * box. Everything reusable lives in the toolkit (textarea, modal, prompt, message, file dialog).
 */
#include "nwui.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#define CAP 65536

static char        g_text[CAP];                 /* the document (the textarea's buffer) */
static char        g_undo[CAP], g_shadow[CAP];   /* single-level undo: prev + lagging shadow */
static int         g_have_undo;
static char        g_path[256];                  /* current file (empty = Untitled) */
static int         g_dirty;
static char        g_find[128], g_repl[128];      /* find / replace strings */
static int         g_matchcase;
static char        g_gotobuf[16];
static nwui       *g_u;
static nwui_node  *g_ta, *g_status;
static int         g_wrap, g_show_status = 1;

static const char *last_component(const char *p)
{
	const char *s = p;
	for (const char *q = p; *q; q++) if (*q == '/') s = q + 1;
	return s;
}

static void update_status(void)
{
	int ln, col;
	nwui_textarea_caret(g_ta, &ln, &col);
	const char *nm = g_path[0] ? last_component(g_path) : "Untitled";
	char b[128];
	snprintf(b, sizeof b, "%s%s    Ln %d, Col %d", g_dirty ? "*" : "", nm, ln, col);
	nwui_set_text(g_status, g_show_status ? b : "");
}

/* single-level undo: g_shadow lags one edit behind, so on_change can record the pre-edit state */
static void on_change(nwui_node *self, void *u)
{
	(void) self; (void) u;
	memcpy(g_undo, g_shadow, CAP);     /* undo target = the buffer before this change */
	memcpy(g_shadow, g_text, CAP);     /* shadow now mirrors the current buffer */
	g_have_undo = 1;
	g_dirty = 1;
	update_status();
}
static void reset_undo(void) { memcpy(g_shadow, g_text, CAP); g_have_undo = 0; }

/* ---- File ---- */
static void do_load(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) { nwui_message(g_u, "Notepad", "Cannot open file"); return; }
	int n = (int) read(fd, g_text, CAP - 1);
	close(fd);
	if (n < 0) n = 0;
	g_text[n] = 0;
	strncpy(g_path, path, sizeof g_path - 1); g_path[sizeof g_path - 1] = 0;
	nwui_set_text(g_ta, g_text);       /* reset caret/scroll + re-measure */
	reset_undo();
	g_dirty = 0; update_status();
}
static void do_save(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) { nwui_message(g_u, "Notepad", "Cannot save file"); return; }
	write(fd, g_text, (int) strlen(g_text));
	close(fd);
	strncpy(g_path, path, sizeof g_path - 1); g_path[sizeof g_path - 1] = 0;
	g_dirty = 0; update_status();
}
static void open_ok(nwui_node *s, void *u) { (void) s; (void) u; do_load(g_path); }
static void save_ok(nwui_node *s, void *u) { (void) s; (void) u; do_save(g_path); }

static void m_new(nwui_node *s, void *u)  { (void) s; (void) u;
	g_text[0] = 0; g_path[0] = 0;
	nwui_set_text(g_ta, g_text); reset_undo(); g_dirty = 0; update_status(); }
static void m_open(nwui_node *s, void *u) { (void) s; (void) u;
	nwui_file_dialog(g_u, 0, "/disks/main", g_path, sizeof g_path, open_ok, 0); }
static void m_save(nwui_node *s, void *u) { (void) s; (void) u;
	if (g_path[0]) do_save(g_path);
	else nwui_file_dialog(g_u, 1, "/disks/main", g_path, sizeof g_path, save_ok, 0); }
static void m_saveas(nwui_node *s, void *u){ (void) s; (void) u;
	nwui_file_dialog(g_u, 1, "/disks/main", g_path, sizeof g_path, save_ok, 0); }
static void m_exit(nwui_node *s, void *u) { (void) s; (void) u; _exit(0); }

/* ---- Edit ---- */
static void m_undo(nwui_node *s, void *u)  { (void) s; (void) u;
	if (!g_have_undo) return;
	char tmp[CAP];
	memcpy(tmp, g_text, CAP); memcpy(g_text, g_undo, CAP); memcpy(g_undo, tmp, CAP);
	memcpy(g_shadow, g_text, CAP);     /* a second Undo swaps back == redo */
	nwui_set_text(g_ta, g_text); g_dirty = 1; update_status(); }
static void m_cut(nwui_node *s, void *u)   { (void) s; (void) u; nwui_post_copy(g_u, 1); }
static void m_copy(nwui_node *s, void *u)  { (void) s; (void) u; nwui_post_copy(g_u, 0); }
static void m_paste(nwui_node *s, void *u) { (void) s; (void) u; nwui_post_paste(g_u); }
static void m_selall(nwui_node *s, void *u){ (void) s; (void) u; nwui_textarea_select_all(g_ta); }

static void find_ok(nwui_node *s, void *u) { (void) s; (void) u;
	if (!g_find[0]) return;
	if (!nwui_textarea_find(g_ta, g_find, g_matchcase, 1))
		nwui_message(g_u, "Notepad", "Cannot find text"); }
static void m_find(nwui_node *s, void *u)  { (void) s; (void) u;
	nwui_prompt(g_u, "Find", g_find, sizeof g_find, find_ok, 0); }
static void m_findnext(nwui_node *s, void *u){ (void) s; (void) u; find_ok(0, 0); }

static void goto_ok(nwui_node *s, void *u) { (void) s; (void) u;
	int ln = 0;
	for (const char *p = g_gotobuf; *p >= '0' && *p <= '9'; p++) ln = ln * 10 + (*p - '0');
	if (ln > 0) nwui_textarea_goto_line(g_ta, ln); }
static void m_goto(nwui_node *s, void *u)  { (void) s; (void) u;
	nwui_prompt(g_u, "Go To Line", g_gotobuf, sizeof g_gotobuf, goto_ok, 0); }

/* Replace dialog: composed from the modal + primitives (find/replace fields + match-case box). */
static void repl_one(nwui_node *s, void *u)  { (void) s; (void) u;
	if (g_find[0] && nwui_textarea_find(g_ta, g_find, g_matchcase, 1))
		nwui_textarea_insert_text(g_ta, g_repl); }
static void repl_all(nwui_node *s, void *u)  { (void) s; (void) u;
	if (!g_find[0]) return;
	nwui_textarea_goto_line(g_ta, 1);          /* start from the top (caret/anchor -> 0) */
	while (nwui_textarea_find(g_ta, g_find, g_matchcase, 0))
		nwui_textarea_insert_text(g_ta, g_repl); }
static void repl_close(nwui_node *s, void *u){ (void) s; (void) u; nwui_close_modal(g_u); }
static void m_replace(nwui_node *s, void *u) { (void) s; (void) u;
	nwui_node *col = nwui_gap(nwui_pad(nwui_vbox(g_u), 14), 8);
	nwui_add(col, nwui_colors(nwui_label(g_u, "Replace"), 0x172130, 0));
	nwui_add(col, nwui_label(g_u, "Find:"));
	nwui_add(col, nwui_textfield(g_u, g_find, sizeof g_find, 0, 0));
	nwui_add(col, nwui_label(g_u, "Replace with:"));
	nwui_add(col, nwui_textfield(g_u, g_repl, sizeof g_repl, 0, 0));
	nwui_add(col, nwui_checkbox(g_u, "Match case", &g_matchcase, 0, 0));
	nwui_node *btns = nwui_gap(nwui_hbox(g_u), 8);
	nwui_add(btns, nwui_button(g_u, "Replace", repl_one, 0));
	nwui_add(btns, nwui_button(g_u, "Replace All", repl_all, 0));
	nwui_add(btns, nwui_button(g_u, "Close", repl_close, 0));
	nwui_add(col, btns);
	nwui_colors(col, 0, 0x00ffffff);
	nwui_open_modal(g_u, col, 0, 0); }

/* ---- Format / View / Help ---- */
static void m_wrap(nwui_node *s, void *u)  { (void) s; (void) u;
	g_wrap = !g_wrap; nwui_textarea_set_wrap(g_ta, g_wrap); }
static void m_statusbar(nwui_node *s, void *u){ (void) s; (void) u;
	g_show_status = !g_show_status; update_status(); }
static void m_timedate(nwui_node *s, void *u){ (void) s; (void) u;
	time_t t = time(0); struct tm *tmv = localtime(&t);
	char b[32];
	if (tmv) strftime(b, sizeof b, "%H:%M %Y-%m-%d", tmv); else b[0] = 0;
	nwui_textarea_insert_text(g_ta, b); update_status(); }
static void m_about(nwui_node *s, void *u) { (void) s; (void) u;
	nwui_message(g_u, "About Notepad", "NanOS Notepad - a libnwui demo editor"); }

int main(int argc, char **argv)
{
	nwui *u = nwui_open("Notepad", 560, 420);
	if (!u) return 1;
	g_u = u;
	g_ta = nwui_textarea(u, g_text, CAP, on_change, 0);
	g_status = nwui_label(u, "Ln 1, Col 1");

	int mf = nwui_menu(u, "File");
	nwui_menu_item(u, mf, "New", m_new, 0);
	nwui_menu_item(u, mf, "Open...", m_open, 0);
	nwui_menu_item(u, mf, "Save", m_save, 0);
	nwui_menu_item(u, mf, "Save As...", m_saveas, 0);
	nwui_menu_separator(u, mf);
	nwui_menu_item(u, mf, "Exit", m_exit, 0);

	int me = nwui_menu(u, "Edit");
	nwui_menu_item(u, me, "Undo", m_undo, 0);
	nwui_menu_separator(u, me);
	nwui_menu_item(u, me, "Cut", m_cut, 0);
	nwui_menu_item(u, me, "Copy", m_copy, 0);
	nwui_menu_item(u, me, "Paste", m_paste, 0);
	nwui_menu_separator(u, me);
	nwui_menu_item(u, me, "Find...", m_find, 0);
	nwui_menu_item(u, me, "Find Next", m_findnext, 0);
	nwui_menu_item(u, me, "Replace...", m_replace, 0);
	nwui_menu_item(u, me, "Go To...", m_goto, 0);
	nwui_menu_item(u, me, "Select All", m_selall, 0);
	nwui_menu_item(u, me, "Time/Date", m_timedate, 0);

	int mfo = nwui_menu(u, "Format");
	nwui_menu_item(u, mfo, "Word Wrap", m_wrap, 0);
	int mv = nwui_menu(u, "View");
	nwui_menu_item(u, mv, "Status Bar", m_statusbar, 0);
	int mh = nwui_menu(u, "Help");
	nwui_menu_item(u, mh, "About Notepad", m_about, 0);

	/* accelerators reuse the menu callbacks (Ctrl tracked client-side by the toolkit) */
	nwui_accel(u, 1, 'n', 0, m_new, 0);     nwui_accel(u, 1, 'o', 0, m_open, 0);
	nwui_accel(u, 1, 's', 0, m_save, 0);    nwui_accel(u, 1, 'f', 0, m_find, 0);
	nwui_accel(u, 1, 'h', 0, m_replace, 0); nwui_accel(u, 1, 'g', 0, m_goto, 0);
	nwui_accel(u, 1, 'a', 0, m_selall, 0);  nwui_accel(u, 1, 'z', 0, m_undo, 0);
	nwui_accel(u, 1, 'x', 0, m_cut, 0);     nwui_accel(u, 1, 'c', 0, m_copy, 0);
	nwui_accel(u, 1, 'v', 0, m_paste, 0);
	nwui_accel(u, 0, 0, NWUI_KEY_F3, m_findnext, 0);
	nwui_accel(u, 0, 0, NWUI_KEY_F5, m_timedate, 0);

	nwui_node *col = nwui_vbox(u);
	nwui_add(col, nwui_flex(g_ta, 1));
	nwui_node *bar = nwui_pad(nwui_hbox(u), 2);
	nwui_add(bar, g_status);
	nwui_colors(bar, 0, 0x00eef3f9);
	nwui_add(col, bar);
	nwui_set_root(u, col);
	nwui_focus(u, g_ta);                   /* editor focused at start */

	reset_undo();
	/* "Open with": if launched with a file-path argument (e.g. from the file manager), load it. */
	if (argc > 1 && argv[1] && argv[1][0])
		do_load(argv[1]);
	update_status();
	nwui_run(u);
	return 0;
}
