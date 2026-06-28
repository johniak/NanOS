/*
 * open.c — `open <file>`: the macOS-style launcher, runnable from a terminal that lives in the
 * NanOS desktop. It resolves the file's associated app (the SAME launch.h logic the GUI file
 * manager uses) and asks the compositor to start it, by connecting to nwm's AF_UNIX launch
 * socket (see nwspawn.h). So the one "open" serves both the UI (libnwui's nwui_open_file) and
 * the command line — only the transport differs (a window client uses its own connection; a
 * terminal child uses the socket).
 *
 *   open report.txt      # opens it in the associated editor
 *   open photo.png       # opens it in the image viewer
 *   open /path/app.nxe   # runs a program directly
 *
 * Requires a running desktop (nwm); from a plain text console there is nothing to launch into.
 */
#include "launch.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char **argv)
{
	if (argc < 2 || !argv[1] || !argv[1][0]) {
		fprintf(stderr, "usage: open <file|app.nxe>\n");
		return 2;
	}
	const char *path = argv[1];
	int l = (int) strlen(path);
	int is_nxe = (l > 4 && !strcmp(path + l - 4, ".nxe"));

	char app[128];
	if (is_nxe) {
		strncpy(app, path, sizeof app - 1); app[sizeof app - 1] = 0;   /* run the program directly */
	} else {
		char ext[16];
		nw_file_ext(path, ext, sizeof ext);
		if (!nw_assoc_lookup(ext, app, sizeof app)) {
			fprintf(stderr, "open: no application is associated with '.%s'\n", ext[0] ? ext : "");
			return 1;
		}
	}

	/* hand the launch to the desktop (no password -> a normal, unprivileged launch) */
	if (nw_launch_send(app, is_nxe ? "" : path, "") != 0) {
		fprintf(stderr, "open: no desktop session to open into (is nwm running?)\n");
		return 1;
	}
	return 0;
}
