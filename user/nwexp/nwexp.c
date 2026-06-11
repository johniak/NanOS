/*
 * nwexp.c — a simple file explorer for NanWM, built on libnwui's list widget.
 *
 * Lists a directory (opendir/readdir over the kernel's getdents64), one row per entry with a
 * trailing "/" on sub-directories. A single click selects; a double-click (or Enter) opens:
 * a directory is entered (".." climbs to the parent), a ".nxe" program is launched via the
 * compositor (nwui_spawn). The path label tracks the current directory. The whole UI — list,
 * focus, keyboard nav, scrollbar, double-click timing — comes from the toolkit; this app is
 * just the tree + a little state.
 */
#include "nwui.h"
#include <dirent.h>
#include <string.h>
#include <stdio.h>

enum { MAX_ENT = 256, NAME_CAP = 64, DT_DIR = 4 };

static char        g_cwd[256] = "/disks/main";
static char        g_names[MAX_ENT][NAME_CAP];   /* display rows (dirs end with '/') */
static char        g_is_dir[MAX_ENT];            /* parallel: 1 = directory, ".." counts as dir */
static const char *g_items[MAX_ENT];             /* pointers into g_names for nwui_list_set */
static int         g_count;

static nwui       *g_ui;
static nwui_node  *g_list;
static nwui_node  *g_path;

/* True if `name` looks like an executable program (a .nxe binary). */
static int is_nxe(const char *name)
{
	int n = (int) strlen(name);
	return n > 4 && strcmp(name + n - 4, ".nxe") == 0;
}

/* Build the absolute path of child `name` under g_cwd into `out`. */
static void join_path(char *out, int cap, const char *dir, const char *name)
{
	if (dir[0] == '/' && dir[1] == 0)            /* root: avoid a doubled slash */
		snprintf(out, cap, "/%s", name);
	else
		snprintf(out, cap, "%s/%s", dir, name);
}

/* Replace g_cwd with its parent directory (drop the last component). */
static void go_parent(void)
{
	int n = (int) strlen(g_cwd);
	while (n > 1 && g_cwd[n - 1] != '/') n--;    /* back up to the slash */
	if (n > 1) n--;                              /* drop the slash itself */
	if (n < 1) n = 1;                            /* never below "/" */
	g_cwd[n] = 0;
}

static void load_dir(const char *path)
{
	DIR *d = opendir(path);
	if (!d)
		return;                                  /* keep the old listing on failure */

	if ((int) strlen(path) < (int) sizeof g_cwd)
		strcpy(g_cwd, path);

	g_count = 0;
	if (!(g_cwd[0] == '/' && g_cwd[1] == 0)) {   /* offer ".." everywhere but the root */
		strcpy(g_names[g_count], "..");
		g_is_dir[g_count] = 1;
		g_items[g_count] = g_names[g_count];
		g_count++;
	}

	struct dirent *e;
	while ((e = readdir(d)) != 0 && g_count < MAX_ENT) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		int isdir = (e->d_type == DT_DIR);
		char *row = g_names[g_count];
		int k = 0;
		for (; e->d_name[k] && k < NAME_CAP - 2; k++)
			row[k] = e->d_name[k];
		if (isdir && k < NAME_CAP - 2) row[k++] = '/';   /* mark directories */
		row[k] = 0;
		g_is_dir[g_count] = (char) isdir;
		g_items[g_count]  = row;
		g_count++;
	}
	closedir(d);

	nwui_list_set(g_list, g_items, g_count);
	nwui_set_text(g_path, g_cwd);
}

static void on_activate(nwui_node *self, void *user)
{
	int sel = nwui_list_selected(g_list);
	if (sel < 0 || sel >= g_count)
		return;
	if (!g_is_dir[sel]) {                        /* a file: run it if it's a program */
		if (is_nxe(g_names[sel])) {
			char target[256];
			join_path(target, sizeof target, g_cwd, g_names[sel]);
			nwui_spawn(g_ui, target);            /* ask the compositor to launch it */
		}
		return;
	}

	if (strcmp(g_names[sel], "..") == 0) {
		go_parent();                             /* edits g_cwd in place */
		char target[256];
		strcpy(target, g_cwd);
		load_dir(target);
	} else {
		char name[NAME_CAP];
		int k = 0;
		for (; g_names[sel][k] && g_names[sel][k] != '/'; k++)   /* strip the trailing '/' */
			name[k] = g_names[sel][k];
		name[k] = 0;
		char target[256];
		join_path(target, sizeof target, g_cwd, name);
		load_dir(target);
	}
}

int main(void)
{
	nwui *u = nwui_open("Files", 360, 280);
	if (!u)
		return 1;
	g_ui = u;

	g_path = nwui_label(u, g_cwd);
	g_list = nwui_list(u, on_activate, 0);

	nwui_set_root(u, nwui_pad(nwui_gap(nwui_column(u,
		g_path,
		nwui_flex(g_list, 1),
		(nwui_node *) 0), 8), 12));

	load_dir(g_cwd);
	nwui_run(u);
	return 0;
}
