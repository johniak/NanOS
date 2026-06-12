/*
 * sys/utime.h — struct utimbuf + utime(). Overlays picolibc's "dummy" newlib copy, which
 * references time_t without including its definition (breaks gnulib's utime.c). NanOS implements
 * utime() over SYS_utime (libc-glue/syscalls.c), so it's declared unconditionally here.
 */
#ifndef _SYS_UTIME_H
#define _SYS_UTIME_H

#include <sys/types.h>   /* time_t */

#ifdef __cplusplus
extern "C" {
#endif

struct utimbuf {
	time_t actime;   /* access time */
	time_t modtime;  /* modification time */
};

int utime(const char* path, const struct utimbuf* times);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_UTIME_H */
