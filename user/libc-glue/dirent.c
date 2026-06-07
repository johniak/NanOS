/*
 * dirent.c — opendir/readdir/closedir over the kernel getdents64 syscall.
 *
 * The kernel returns Linux dirent64 records: d_ino at byte 0, d_reclen (u16) at
 * byte 16, d_type (u8) at byte 18, the NUL-terminated name at byte 19, each record
 * 8-byte aligned (see kernel/Syscall.cpp). We parse those into our struct dirent.
 *
 * NOTE: the kernel's getdents64 has no read cursor — it returns the WHOLE directory
 * (truncated to the buffer) on every call and never signals EOF. So we read it ONCE
 * in opendir and then just walk the buffer; calling it again would loop forever.
 * Directories larger than DIRBUFSZ are therefore truncated (fine for NanOS today).
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
	char abs[256];
	nx_resolve(path, abs);
	int fd = sys3(SYS_open, (int) abs, 0, 0);
	if (fd < 0)
		return 0;
	DIR* d = (DIR*) malloc(sizeof(DIR));
	if (!d) {
		sys3(SYS_close, fd, 0, 0);
		return 0;
	}
	d->fd = fd;
	d->bufpos = 0;
	int n = sys3(SYS_getdents64, fd, (int) d->buf, DIRBUFSZ);   // read all entries once
	d->buflen = n > 0 ? n : 0;
	return d;
}

struct dirent* readdir(DIR* d) {
	if (d->bufpos >= d->buflen)
		return 0;                       // exhausted: EOF (no refill — see note above)
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
