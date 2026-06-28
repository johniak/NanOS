/*
 * launch.h — the shared "open"/LaunchServices logic for NanOS, used by BOTH the GUI toolkit
 * (libnwui's nwui_open_file) AND the standalone `open` command, so the UI and the terminal open
 * files the same way (macOS-style). Two pure pieces:
 *   - nw_file_ext()     — the lowercase extension of a path
 *   - nw_assoc_lookup()  — extension -> app name, from /disks/main/nanos/config/associations.conf
 *                          (':' / '=' / space separated, '#' comments), with built-in defaults.
 * Plus the well-known nwm spawn-socket path the `open` command connects to.
 *
 * Header-only (static) so it needs no extra library — the CLI links only libc; libnwui includes
 * it directly. Keeping it here, not in the toolkit, is deliberate: a CLI must not drag in the GUI.
 */
#ifndef NW_LAUNCH_H
#define NW_LAUNCH_H

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "nwspawn.h"   /* NW_SPAWN_SOCK — the compositor's launch socket */

/* Where the editable associations live (the "Default Apps" Settings panel writes this). */
#define NW_ASSOC_PATH  "/disks/main/nanos/config/associations.conf"

/* Lowercased extension (no dot) of `path` into ext[cap]; "" if none. */
static void nw_file_ext(const char *path, char *ext, int cap)
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

/* Built-in default associations (used when the config has no entry for an extension). */
static const struct { const char *ext, *app; } NW_DEFAULT_ASSOC[] = {
	{ "txt", "nwnote" }, { "c", "nwnote" }, { "h", "nwnote" }, { "md", "nwnote" },
	{ "cfg", "nwnote" }, { "conf", "nwnote" }, { "rs", "nwnote" }, { "sh", "nwnote" },
	{ "log", "nwnote" }, { "yaml", "nwnote" }, { "ini", "nwnote" }, { "json", "nwnote" },
	{ "png", "nwview" },
};

/* Resolve `ext` (lowercase, no dot) -> app name into out[cap]. Config overrides built-ins. 1/0. */
static int nw_assoc_lookup(const char *ext, char *out, int cap)
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
	for (int i = 0; i < (int) (sizeof NW_DEFAULT_ASSOC / sizeof NW_DEFAULT_ASSOC[0]); i++)
		if (!strcmp(ext, NW_DEFAULT_ASSOC[i].ext)) {
			int k = 0; for (; NW_DEFAULT_ASSOC[i].app[k] && k < cap - 1; k++) out[k] = NW_DEFAULT_ASSOC[i].app[k];
			out[k] = 0;
			return 1;
		}
	return 0;
}

/* Ask the desktop (nwm) to launch `cmd` with `arg` as argv[1]. If `mode` is "elevate", the launch
 * is run as administrator: the COMPOSITOR pops its modal system auth dialog, collects the admin
 * password itself, and (on success) runs the app as root via nwsu — the client never handles the
 * password. Sends "cmd\0arg\0mode" over the launch socket. Returns 0 on success, -1 if the desktop
 * isn't reachable. */
#define NW_LAUNCH_NORMAL  ""
#define NW_LAUNCH_ELEVATE "elevate"
static int nw_launch_send(const char *cmd, const char *arg, const char *mode)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	struct sockaddr_un sa;
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, NW_SPAWN_SOCK, sizeof sa.sun_path - 1);
	if (connect(fd, (struct sockaddr *) &sa, sizeof sa) < 0) { close(fd); return -1; }
	char msg[768];
	int n = 0;
	for (const char *p = cmd; *p && n < (int) sizeof msg - 1; p++) msg[n++] = *p;
	msg[n++] = 0;
	if (arg) for (const char *p = arg; *p && n < (int) sizeof msg - 1; p++) msg[n++] = *p;
	msg[n++] = 0;
	if (mode) for (const char *p = mode; *p && n < (int) sizeof msg - 1; p++) msg[n++] = *p;
	int off = 0, w;
	while (off < n && (w = (int) write(fd, msg + off, n - off)) > 0) off += w;
	close(fd);
	return off == n ? 0 : -1;
}

#endif /* NW_LAUNCH_H */
