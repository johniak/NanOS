/*
 * dirent.c — opendir/readdir/closedir over the kernel getdents64 syscall.
 *
 * The kernel returns Linux dirent64 records: d_ino at byte 0, d_reclen (u16) at
 * byte 16, d_type (u8) at byte 18, the NUL-terminated name at byte 19, each record
 * 8-byte aligned (see kernel/Syscall.cpp). We parse those into our struct dirent.
 *
 * The kernel's getdents64 is a cursor: each call fills the buffer with the next batch of
 * records and returns 0 only at true end-of-directory (see kernel/Syscall.cpp). So we refill
 * on demand and loop until EOF — directories larger than DIRBUFSZ are NOT truncated.
 */
#include <dirent.h>
#include <stdlib.h>
#include "SyscallNr.h"

static inline int sys3(int nr, int a, int b, int c) {
	int r;
	__asm__ __volatile__("int $0x80" : "=a"(r) : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory");
	return r;
}

#define DIRBUFSZ 8192

struct __dirstream {
	int fd;
	int bufpos;
	int buflen;
	char buf[DIRBUFSZ];
	struct dirent de;
};

DIR* opendir(const char* path) {
	int fd = sys3(SYS_open, (int) path, 0, 0);   // kernel resolves relative paths vs the cwd
	if (fd < 0)
		return 0;
	DIR* d = (DIR*) malloc(sizeof(DIR));
	if (!d) {
		sys3(SYS_close, fd, 0, 0);
		return 0;
	}
	d->fd = fd;
	d->bufpos = 0;
	d->buflen = 0;                      // first readdir() pulls the first batch
	return d;
}

struct dirent* readdir(DIR* d) {
	if (d->bufpos >= d->buflen) {
		int n = sys3(SYS_getdents64, d->fd, (int) d->buf, DIRBUFSZ);   // next batch
		if (n <= 0)
			return 0;                   // 0 = EOF, <0 = error
		d->buflen = n;
		d->bufpos = 0;
	}
	char* rec = d->buf + d->bufpos;
	unsigned short reclen = *(unsigned short*) (rec + 16);
	d->de.d_ino = *(unsigned*) (rec + 0);
	d->de.d_type = (unsigned char) rec[18];
	int i = 0;
	for (; i < 255 && rec[19 + i]; i++)
		d->de.d_name[i] = rec[19 + i];
	d->de.d_name[i] = 0;
	d->bufpos += reclen ? reclen : (int) sizeof(struct dirent);
	return &d->de;
}

int closedir(DIR* d) {
	int fd = d->fd;
	free(d);
	return sys3(SYS_close, fd, 0, 0);
}

/* dirfd: the underlying file descriptor (so callers can fstat/openat relative to the dir). */
int dirfd(DIR* d) { return d->fd; }

/* rewinddir: seek the directory fd back to the start and drop the read-ahead buffer, so the
 * next readdir() re-reads from the first entry. */
void rewinddir(DIR* d) {
	sys3(SYS_lseek, d->fd, 0, 0 /* SEEK_SET */);
	d->bufpos = 0;
	d->buflen = 0;
}

/* fdopendir: wrap an already-open directory fd in a DIR stream (takes ownership of fd). */
DIR* fdopendir(int fd) {
	if (fd < 0)
		return 0;
	DIR* d = (DIR*) malloc(sizeof(DIR));
	if (!d)
		return 0;
	d->fd = fd;
	d->bufpos = 0;
	d->buflen = 0;
	return d;
}
