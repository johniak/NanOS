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

typedef struct __dirstream DIR;

DIR* opendir(const char* path);
struct dirent* readdir(DIR* dir);
int closedir(DIR* dir);

#endif /* _NX_DIRENT_H */
