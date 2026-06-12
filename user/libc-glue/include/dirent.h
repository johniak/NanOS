/*
 * dirent.h — NanOS userland directory access.
 *
 * picolibc's <sys/dirent.h> is a stub that #errors for this target, so we provide
 * our own DIR/struct dirent and opendir/readdir/closedir, implemented over the
 * kernel's getdents64 (see user/libc-glue/dirent.c). This header shadows
 * picolibc's because user/libc-glue/include is earlier on the include path.
 */
#ifndef _NX_DIRENT_H
#define _NX_DIRENT_H

#include <sys/types.h>

struct dirent {
	unsigned long d_ino;
	unsigned char d_type;
	char d_name[256];
};

/* d_type values (Linux <dirent.h>), used by gnulib's fts.c and friends. */
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12
#define DT_WHT     14

typedef struct __dirstream DIR;

DIR* opendir(const char* path);
struct dirent* readdir(DIR* dir);
int closedir(DIR* dir);

/* Declared (not implemented) so gnulib's dir-walking modules (fdopendir/dirfd/rewinddir/
 * scandir, pulled by fts) compile their thin system-wrapper path. NanOS userland doesn't
 * implement these yet; nothing that links them is built (ping never walks directories). */
int dirfd(DIR* dir);
DIR* fdopendir(int fd);
void rewinddir(DIR* dir);
void seekdir(DIR* dir, long loc);
long telldir(DIR* dir);
int scandir(const char* dir, struct dirent*** namelist,
            int (*sel)(const struct dirent*),
            int (*cmp)(const struct dirent**, const struct dirent**));

#endif /* _NX_DIRENT_H */
