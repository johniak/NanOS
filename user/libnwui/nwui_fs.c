/* nwui_fs.c — reusable directory enumeration + file operations for toolkit apps (see nwui_fs.h). */
#include "nwui_fs.h"
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statfs.h>

void *nwui_dir_open(const char *path)
{
	return (void *) opendir(path);
}

int nwui_dir_next(void *d, char *name, int cap, int *is_dir)
{
	DIR *dir = (DIR *) d;
	if (!dir || cap <= 0) return 0;
	struct dirent *e;
	while ((e = readdir(dir)) != 0) {
		if (e->d_name[0] == '.' && (e->d_name[1] == 0 ||
		    (e->d_name[1] == '.' && e->d_name[2] == 0)))
			continue;                       /* skip "." and ".." */
		int k = 0;
		for (; e->d_name[k] && k < cap - 1; k++) name[k] = e->d_name[k];
		name[k] = 0;
		if (is_dir) *is_dir = (e->d_type == DT_DIR);
		return 1;
	}
	return 0;
}

void nwui_dir_close(void *d)
{
	if (d) closedir((DIR *) d);
}

/* ---- file operations ---- */

int nwui_fs_mkdir(const char *path) { return mkdir(path, 0755) == 0 ? 0 : -1; }
int nwui_fs_rename(const char *from, const char *to) { return rename(from, to) == 0 ? 0 : -1; }

int nwui_fs_exists(const char *path)
{
	struct stat st;
	return lstat(path, &st) == 0 ? 1 : 0;
}

int nwui_fs_isdir(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0) return -1;
	return S_ISDIR(st.st_mode) ? 1 : 0;
}

long nwui_fs_size(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
	return (long) st.st_size;
}

int nwui_fs_space(const char *path, unsigned long long *avail, unsigned long long *total)
{
	struct statfs s;
	if (statfs(path, &s) != 0) return -1;
	unsigned long long bs = (unsigned long long) (s.f_bsize ? s.f_bsize : 4096);
	if (avail) *avail = (unsigned long long) s.f_bavail * bs;
	if (total) *total = (unsigned long long) s.f_blocks * bs;
	return 0;
}

/* Join dir + "/" + name into out (cap bytes). Returns 1 on success, 0 if it would overflow. */
static int join(const char *dir, const char *name, char *out, int cap)
{
	int n = snprintf(out, cap, "%s/%s", dir, name);
	return n > 0 && n < cap;
}

/* Recursively delete `path` (file, symlink, or directory tree). 0 = fully removed, -1 = error. */
int nwui_fs_remove(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0) return -1;
	if (!S_ISDIR(st.st_mode))                /* file or symlink: a plain unlink */
		return unlink(path) == 0 ? 0 : -1;

	DIR *dir = opendir(path);
	if (!dir) return -1;
	int rc = 0;
	struct dirent *e;
	char child[1024];
	while ((e = readdir(dir)) != 0) {
		if (e->d_name[0] == '.' && (e->d_name[1] == 0 ||
		    (e->d_name[1] == '.' && e->d_name[2] == 0)))
			continue;
		if (!join(path, e->d_name, child, sizeof child)) { rc = -1; continue; }
		if (nwui_fs_remove(child) != 0) rc = -1;
	}
	closedir(dir);
	if (rmdir(path) != 0) rc = -1;
	return rc;
}

/* Stream-copy a regular file from -> to, preserving the mode bits. 0 / -1. */
static int copy_file(const char *from, const char *to, mode_t mode)
{
	int in = open(from, O_RDONLY);
	if (in < 0) return -1;
	int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
	if (out < 0) { close(in); return -1; }
	char buf[8192];
	int n, rc = 0;
	while ((n = (int) read(in, buf, sizeof buf)) > 0) {
		int off = 0;
		while (off < n) {
			int w = (int) write(out, buf + off, n - off);
			if (w <= 0) { rc = -1; goto done; }
			off += w;
		}
	}
	if (n < 0) rc = -1;
done:
	close(in);
	close(out);
	return rc;
}

/* Recursively copy `from` -> `to` (must not already exist). Handles file, symlink, directory. */
int nwui_fs_copy(const char *from, const char *to)
{
	struct stat st;
	if (lstat(from, &st) != 0) return -1;

	if (S_ISLNK(st.st_mode)) {               /* recreate the symlink, do not follow it */
		char target[1024];
		long n = readlink(from, target, sizeof target - 1);
		if (n < 0) return -1;
		target[n] = 0;
		return symlink(target, to) == 0 ? 0 : -1;
	}
	if (!S_ISDIR(st.st_mode))
		return copy_file(from, to, st.st_mode);

	if (mkdir(to, st.st_mode & 0777) != 0) return -1;
	DIR *dir = opendir(from);
	if (!dir) return -1;
	int rc = 0;
	struct dirent *e;
	char cf[1024], ct[1024];
	while ((e = readdir(dir)) != 0) {
		if (e->d_name[0] == '.' && (e->d_name[1] == 0 ||
		    (e->d_name[1] == '.' && e->d_name[2] == 0)))
			continue;
		if (!join(from, e->d_name, cf, sizeof cf) ||
		    !join(to,   e->d_name, ct, sizeof ct)) { rc = -1; continue; }
		if (nwui_fs_copy(cf, ct) != 0) rc = -1;
	}
	closedir(dir);
	return rc;
}
