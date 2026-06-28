/*
 * nw_settings_path.h — resolve WHERE the desktop-preferences file lives.
 *
 * The compositor (nwm) and the Settings app both run as the logged-in desktop user (the greeter
 * drops to that uid before exec), so the live, user-editable preferences live in $HOME — exactly
 * where that unprivileged user can both read AND write them. The system config dir
 * /disks/main/nanos/config is root-owned (it holds passwd/shadow/sudoers) and is NOT writable by
 * the desktop, so a user-owned file there cannot be created — writes would silently fail and no
 * preference change would ever take effect. This mirrors the $HOME/.nanos-open per-file store.
 *
 * NW_SETTINGS_PATH (the system path) remains a read-only, shipped-default location: an admin may
 * place a system-wide settings.yaml there and every user's file overlays on top of it.
 *
 * Header-only static helper (does I/O via getenv) — used by the userland sides (nwm, settings),
 * NOT by the pure host-tested nw_settings.{h,c}.
 */
#ifndef NW_SETTINGS_PATH_H
#define NW_SETTINGS_PATH_H

#include <stdlib.h>          /* getenv */
#include "nw_settings.h"     /* NW_SETTINGS_PATH (system-wide default fallback) */

/* The per-user preferences file name, appended to $HOME. */
#define NW_SETTINGS_USER_NAME "/.nanos-settings.yaml"

/* Build "$HOME/.nanos-settings.yaml" into out[cap] (falls back to /disks/main if HOME is unset,
 * e.g. nwm launched outside a login session). Returns the byte length written. */
static int nw_settings_user_path(char *out, int cap)
{
	const char *h = getenv("HOME");
	if (!h || !h[0]) h = "/disks/main";
	int n = 0;
	for (; h[n] && n < cap - 1; n++) out[n] = h[n];
	for (const char *s = NW_SETTINGS_USER_NAME; *s && n < cap - 1; s++) out[n++] = *s;
	out[n] = 0;
	return n;
}

#endif /* NW_SETTINGS_PATH_H */
