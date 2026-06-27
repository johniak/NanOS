/*
 * nwui_fs.h — a tiny reusable directory-enumeration shim for toolkit apps. Wraps opendir/readdir/
 * closedir behind a flat ABI (opaque handle + name/is_dir out-params) so non-C apps (e.g. the Rust
 * file explorer) can list directories without depending on the libc `struct dirent` layout.
 */
#ifndef NWUI_FS_H
#define NWUI_FS_H

/* Open a directory for enumeration. Returns an opaque handle, or 0 on failure. */
void *nwui_dir_open(const char *path);

/* Read the next entry into `name` (NUL-terminated, truncated to `cap`); sets *is_dir to 1 for a
 * directory. Returns 1 on a returned entry, 0 at end-of-directory. Skips "." and "..". */
int nwui_dir_next(void *d, char *name, int cap, int *is_dir);

/* Close a handle from nwui_dir_open. */
void nwui_dir_close(void *d);

#endif /* NWUI_FS_H */
