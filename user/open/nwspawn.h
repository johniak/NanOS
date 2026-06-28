/*
 * nwspawn.h — the well-known AF_UNIX socket the compositor (nwm) listens on as a "launch channel".
 * A process that is NOT a window client (e.g. `open` run from a terminal) connects here and sends
 * a launch request so the desktop starts the app — the macOS `open` -> launchd/WindowServer path.
 *
 * Request wire format (one per connection, then close): "cmd\0arg" — `cmd` is an app name or an
 * absolute .nxe path (resolved like the Run dialog); the optional `arg` after the NUL becomes the
 * program's argv[1] (the file to open). Kept separate from launch.h so nwm needs only this path.
 */
#ifndef NW_SPAWN_H
#define NW_SPAWN_H
#define NW_SPAWN_SOCK  "/tmp/.nwm-spawn"
#endif
