/*
 * nwui.c — toolkit lifecycle + the event loop, over libnw. The only part doing I/O:
 * nwui_open() connects + creates the window, nwui_run() pumps nw_next_event -> nwui_dispatch
 * (core) -> nwui_render (paint) -> nw_commit (damage). The app never touches libnw directly.
 */
#include "nwui_core.h"
#include "libnw.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

struct nwui_io { nw_display *d; nw_win *win; };

/* Milliseconds on the monotonic clock — fed to the core so it can time double-clicks. */
static int now_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (int) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void nwui_spawn(nwui *u, const char *cmd)
{
	struct nwui_io *io = (struct nwui_io *) u->io;
	nw_spawn(io->d, cmd);
}

void nwui_spawn_arg(nwui *u, const char *cmd, const char *arg)
{
	struct nwui_io *io = (struct nwui_io *) u->io;
	nw_spawn_arg(io->d, cmd, arg);
}

/* ---- "open" (macOS-style): launch a file in its associated app -------------------------------
 * The reusable LaunchServices of NanOS. The extension->app RESOLUTION lives in the shared
 * launch.h (also used by the standalone `open` command), so the UI and the terminal open files
 * the same way. Here we just resolve + launch via this app's existing compositor connection. */
#include "open/launch.h"

/* Resolve `ext` -> app name (config overrides built-ins). Exposed for Settings' Default Apps. */
int nwui_assoc_lookup(const char *ext, char *out, int cap) { return nw_assoc_lookup(ext, out, cap); }

void nwui_open_file(nwui *u, const char *path)
{
	int l = (int) strlen(path);
	if (l > 4 && !strcmp(path + l - 4, ".nxe")) { nwui_spawn(u, path); return; }   /* a program: run it */
	char ext[16]; nw_file_ext(path, ext, sizeof ext);
	char app[64];
	if (!nwui_assoc_lookup(ext, app, sizeof app)) {
		nwui_message(u, "Open", "No application is associated with this file type.");
		return;
	}
	/* Permissions: the opened app runs as the current user (uid inherited from the session) and
	 * reads the file with that identity. Check readability up front so a permission problem is a
	 * clear message here rather than a cryptic failure inside the launched app. */
	if (access(path, R_OK) != 0) {
		nwui_message(u, "Open", "Permission denied - you do not have access to this file.");
		return;
	}
	nwui_spawn_arg(u, app, path);
}

void nwui_reload_settings(nwui *u)
{
	struct nwui_io *io = (struct nwui_io *) u->io;
	nw_reload_settings(io->d);
}

nwui *nwui_open(const char *title, int w, int h)
{
	nw_display *d = nw_connect();
	if (!d)
		return 0;
	nw_win *win = nw_create_window(d, w, h, title);
	if (!win)
		return 0;
	nwui *u = (nwui *) malloc(sizeof *u);
	if (!u)
		return 0;
	nwui_init(u);
	struct nwui_io *io = (struct nwui_io *) malloc(sizeof *io);
	if (!io) { free(u); return 0; }
	io->d = d; io->win = win;
	u->io = io;
	u->win_w = nw_win_width(win);
	u->win_h = nw_win_height(win);
	return u;
}

static void paint(nwui *u)
{
	struct nwui_io *io = (struct nwui_io *) u->io;
	struct nw_surface s;
	nw_win_surface(io->win, &s);
	int x, y, w, h;
	if (nwui_render(u, &s, &x, &y, &w, &h))
		nw_commit(io->win, x, y, w, h);
}

void nwui_run(nwui *u)
{
	struct nwui_io *io = (struct nwui_io *) u->io;
	if (u->nappmenu > 0) {                      /* publish the app menu to the global bar */
		char spec[512];
		nwui_menu_encode(u, spec, sizeof spec);
		nw_set_menu(io->d, spec);
	}
	u->layout_dirty = 1;
	paint(u);                                  /* first frame */
	struct nw_event ev;
	for (;;) {
		int r = nw_next_event(io->d, &ev, -1); /* block until something happens */
		if (r < 0)
			break;                             /* compositor gone */
		if (r == 0)
			continue;
		/* COALESCE: a scrollbar drag (or fast typing) floods pointer/key events. Process every
		 * one that is already queued, then paint+commit ONCE — repainting after each event would
		 * push a full list repaint through the pipe per mouse-move and lag badly. This mirrors the
		 * compositor's own coalesce-then-render-once loop. */
		int alive = 1;
		do {
			if (ev.type == NW_EV_MENU) {       /* a global-menu item was chosen */
				nwui_menu_dispatch(u, ev.menu, ev.item);
			} else {
				/* A resize must realloc the client draw buffer to the new size BEFORE we relayout
				 * and repaint at it — otherwise paint() keeps drawing into the old (smaller) buffer
				 * and everything past the old bounds vanishes when the window is enlarged. */
				if (ev.type == NW_EV_CONFIGURE)
					nw_win_resize(io->win, ev.x, ev.y);
				u->now_ms = now_ms();          /* stamp time so the core can detect double-clicks */
				if (!nwui_dispatch(u, &ev)) { alive = 0; break; }   /* CLOSE */
				if (u->clip_set) { nw_set_clipboard(io->d, u->clip_buf, u->clip_len); u->clip_set = 0; }
				if (u->clip_get) { nw_get_clipboard(io->d); u->clip_get = 0; }
				if (u->drag_req) { nw_drag_begin(io->d, u->drag_buf, u->drag_len); u->drag_req = 0; }
			}
		} while (nw_next_event(io->d, &ev, 0) > 0);   /* drain the rest, non-blocking */
		if (!alive)
			break;
		paint(u);
	}
}

/* ---- file open/save dialog (I/O: lists a directory via getdents) ---- */
#define FD_MAX 128
static struct {
	nwui *u; int save;
	char  dir[256];
	char  namebuf[FD_MAX][64];
	char *namep[FD_MAX];
	int   n;
	nwui_node *list, *field;
	char *out; int cap;
	nwui_cb on_ok; void *user;
} g_fd;

static void fd_relist(void)
{
	g_fd.n = 0;
	if (g_fd.dir[1]) {                                /* not the root: offer ".." to ascend */
		strcpy(g_fd.namebuf[g_fd.n], "..");
		g_fd.namep[g_fd.n] = g_fd.namebuf[g_fd.n];
		g_fd.n++;
	}
	DIR *d = opendir(g_fd.dir);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d)) && g_fd.n < FD_MAX) {
			if (e->d_name[0] == '.' && e->d_name[1] == 0) continue;   /* skip "." */
			if (e->d_name[0] == '.' && e->d_name[1] == '.' && e->d_name[2] == 0) continue;
			int i = g_fd.n++;
			int k = 0;
			for (; e->d_name[k] && k < 63; k++) g_fd.namebuf[i][k] = e->d_name[k];
			g_fd.namebuf[i][k] = 0;
			g_fd.namep[i] = g_fd.namebuf[i];
		}
		closedir(d);
	}
	nwui_list_set(g_fd.list, (const char *const *) g_fd.namep, g_fd.n);
}
static void fd_activate(nwui_node *self, void *unused)   /* double-click / Enter on a row */
{
	(void) self; (void) unused;
	int sel = nwui_list_selected(g_fd.list);
	if (sel < 0 || sel >= g_fd.n) return;
	const char *nm = g_fd.namep[sel];
	if (strcmp(nm, "..") == 0) { nwui_path_up(g_fd.dir); fd_relist(); return; }
	char joined[256];
	nwui_path_join(g_fd.dir, nm, joined, sizeof joined);
	struct stat st;
	if (stat(joined, &st) == 0 && S_ISDIR(st.st_mode)) {
		strcpy(g_fd.dir, joined);
		fd_relist();
	} else {                                          /* a file: fill the path field */
		int k = 0;
		for (; joined[k] && k < g_fd.cap - 1; k++) g_fd.out[k] = joined[k];
		g_fd.out[k] = 0;
		nwui_set_text(g_fd.field, g_fd.out);
	}
}
static void fd_ok(nwui_node *self, void *unused)
{
	(void) self; (void) unused;
	nwui *u = g_fd.u; nwui_cb cb = g_fd.on_ok; void *usr = g_fd.user;
	nwui_close_modal(u);
	if (cb) cb(0, usr);
}
static void fd_cancel(nwui_node *self, void *u) { (void) self; nwui_close_modal((nwui *) u); }

void nwui_file_dialog(nwui *u, int save, const char *start_dir,
                      char *out_path, int cap, nwui_cb on_ok, void *user)
{
	g_fd.u = u; g_fd.save = save; g_fd.out = out_path; g_fd.cap = cap;
	g_fd.on_ok = on_ok; g_fd.user = user;
	int k = 0;
	for (; start_dir[k] && k < 255; k++) g_fd.dir[k] = start_dir[k];
	g_fd.dir[k] = 0;
	if (k == 0) { g_fd.dir[0] = '/'; g_fd.dir[1] = 0; }

	g_fd.list  = nwui_list(u, fd_activate, 0);
	g_fd.field = nwui_textfield(u, out_path, cap, 0, 0);
	nwui_node *col = nwui_gap(nwui_pad(nwui_vbox(u), 12), 8);
	nwui_add(col, nwui_colors(nwui_label(u, save ? "Save As" : "Open"), 0x172130, 0));
	nwui_add(col, nwui_flex(nwui_size(g_fd.list, 300, 170), 1));
	nwui_add(col, g_fd.field);
	nwui_node *btns = nwui_gap(nwui_hbox(u), 8);
	nwui_add(btns, nwui_button(u, save ? "Save" : "Open", fd_ok, 0));
	nwui_add(btns, nwui_button(u, "Cancel", fd_cancel, u));
	nwui_add(col, btns);
	nwui_colors(col, 0, 0x00ffffff);
	fd_relist();
	nwui_open_modal(u, col, 0, 0);
}
