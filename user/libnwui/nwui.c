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
 * The reusable LaunchServices of NanOS: any libnwui app calls nwui_open_file(u, path) and the
 * right app opens. Associations are extension -> app, configurable in Settings via a plain-text
 * table at /disks/main/nanos/config/associations.conf ("ext: app" per line; '#' comments), with
 * built-in defaults as a fallback. A standalone `open` program is a thin wrapper over this. */
#define NW_ASSOC_PATH "/disks/main/nanos/config/associations.conf"

/* Lowercased extension (without the dot) of `path` into ext[cap]; "" if none. */
static void nwui_file_ext(const char *path, char *ext, int cap)
{
	const char *dot = 0;
	for (const char *p = path; *p; p++) {
		if (*p == '/') dot = 0;
		else if (*p == '.') dot = p;
	}
	ext[0] = 0;
	if (!dot) return;
	int i = 0;
	for (const char *p = dot + 1; *p && i < cap - 1; p++)
		ext[i++] = (*p >= 'A' && *p <= 'Z') ? (char) (*p + 32) : *p;
	ext[i] = 0;
}

/* The built-in default associations (used when the config has no entry for an extension). */
static const struct { const char *ext, *app; } NWUI_DEFAULT_ASSOC[] = {
	{ "txt", "nwnote" }, { "c", "nwnote" }, { "h", "nwnote" }, { "md", "nwnote" },
	{ "cfg", "nwnote" }, { "conf", "nwnote" }, { "rs", "nwnote" }, { "sh", "nwnote" },
	{ "log", "nwnote" }, { "yaml", "nwnote" }, { "ini", "nwnote" }, { "json", "nwnote" },
	{ "png", "nwview" },
};
enum { NWUI_NDEFAULT_ASSOC = (int) (sizeof NWUI_DEFAULT_ASSOC / sizeof NWUI_DEFAULT_ASSOC[0]) };

/* Resolve `ext` -> app name into out[cap]. The config file overrides the built-ins. 1/0. */
int nwui_assoc_lookup(const char *ext, char *out, int cap)
{
	if (!ext || !ext[0]) return 0;
	int fd = open(NW_ASSOC_PATH, O_RDONLY);
	if (fd >= 0) {
		char buf[2048];
		int n = (int) read(fd, buf, sizeof buf - 1);
		close(fd);
		if (n > 0) {
			buf[n] = 0;
			for (char *line = buf; line && *line; ) {
				char *nl = strchr(line, '\n');
				if (nl) *nl = 0;
				while (*line == ' ' || *line == '\t') line++;
				if (*line && *line != '#') {
					char *sep = strpbrk(line, ":= \t");
					if (sep) {
						*sep = 0;
						char *app = sep + 1;
						while (*app == ':' || *app == '=' || *app == ' ' || *app == '\t') app++;
						if (!strcmp(line, ext) && *app) {
							int i = 0; for (; app[i] && i < cap - 1; i++) out[i] = app[i];
							out[i] = 0;
							return 1;
						}
					}
				}
				line = nl ? nl + 1 : 0;
			}
		}
	}
	for (int i = 0; i < NWUI_NDEFAULT_ASSOC; i++)
		if (!strcmp(ext, NWUI_DEFAULT_ASSOC[i].ext)) {
			int k = 0; for (; NWUI_DEFAULT_ASSOC[i].app[k] && k < cap - 1; k++) out[k] = NWUI_DEFAULT_ASSOC[i].app[k];
			out[k] = 0;
			return 1;
		}
	return 0;
}

void nwui_open_file(nwui *u, const char *path)
{
	int l = (int) strlen(path);
	if (l > 4 && !strcmp(path + l - 4, ".nxe")) { nwui_spawn(u, path); return; }   /* a program: run it */
	char ext[16]; nwui_file_ext(path, ext, sizeof ext);
	char app[64];
	if (nwui_assoc_lookup(ext, app, sizeof app))
		nwui_spawn_arg(u, app, path);
	else
		nwui_message(u, "Open", "No application is associated with this file type.");
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
