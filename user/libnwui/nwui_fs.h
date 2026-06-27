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

/* ---- file operations (flat ABI for the file explorer; all paths are NUL-terminated) ---- */
/* Create a directory (mode 0755). Returns 0 on success, -1 on error. */
int  nwui_fs_mkdir(const char *path);
/* Rename/move within the filesystem (atomic; works for files and directories). 0 / -1. */
int  nwui_fs_rename(const char *from, const char *to);
/* Does the path exist? 1 / 0. */
int  nwui_fs_exists(const char *path);
/* 1 if path is a directory, 0 if it exists but is not, -1 if it does not exist. (lstat: a
 * symlink reports 0 — it is treated as a file, not followed.) */
int  nwui_fs_isdir(const char *path);
/* File size in bytes, or -1 if missing / not a regular file. */
long nwui_fs_size(const char *path);
/* Recursively delete a file or directory tree. Returns 0 if everything was removed, else -1. */
int  nwui_fs_remove(const char *path);
/* Recursively copy a file, symlink, or directory tree `from` -> `to` (must not exist). 0 / -1. */
int  nwui_fs_copy(const char *from, const char *to);
/* Filesystem free/total space in bytes for the volume holding `path`. Either out-ptr may be 0.
 * Returns 0 on success, -1 on error. */
int  nwui_fs_space(const char *path, unsigned long long *avail, unsigned long long *total);

#endif /* NWUI_FS_H */
