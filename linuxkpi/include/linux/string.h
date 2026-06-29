/*
 * linuxkpi/include/linux/string.h — string/memory ops for the LinuxKPI shim.
 * Forwards to the underlying <string.h>: libc on the host test path, the freestanding
 * include/string.h (memcpy/memset/strlen/strcmp/strstr) on the kext path.
 */
#ifndef _LINUXKPI_LINUX_STRING_H
#define _LINUXKPI_LINUX_STRING_H

#include <linux/types.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Linux spellings not always in the freestanding header. */
static inline void *memmove_(void *d, const void *s, size_t n) {
	unsigned char *dd = (unsigned char *)d;
	const unsigned char *ss = (const unsigned char *)s;
	if (dd < ss) { for (size_t i = 0; i < n; i++) dd[i] = ss[i]; }
	else { for (size_t i = n; i > 0; i--) dd[i - 1] = ss[i - 1]; }
	return d;
}

#ifdef __cplusplus
}
#endif

#endif /* _LINUXKPI_LINUX_STRING_H */

#ifndef _LKPI_STRING_EXTRA
#define _LKPI_STRING_EXTRA
long strscpy(char *d, const char *s, size_t n);
static inline long strncpy_from_user(char *d, const char *s, long n){ long i=0; for(;i<n&&s[i];i++)d[i]=s[i]; if(i<n)d[i]=0; return i; }
#endif

#ifndef _LKPI_STRING_MEM
#define _LKPI_STRING_MEM
#ifdef __cplusplus
extern "C" {
#endif
int memcmp(const void*, const void*, size_t);
void *memchr(const void*, int, size_t);
void *memmove(void*, const void*, size_t);
size_t strnlen(const char*, size_t);
char *strchr(const char*, int);
char *strrchr(const char*, int);
char *strstr(const char*, const char*);
#ifdef __cplusplus
}
#endif
#endif

#ifndef _LKPI_STRING_EXTRA2
#define _LKPI_STRING_EXTRA2
int strncmp(const char*, const char*, size_t);
char *strcpy(char*, const char*);
char *strncpy(char*, const char*, size_t);
static inline long strscpy_pad(char *d, const char *s, size_t n){ long i=0; for(;i<(long)n-1&&s[i];i++)d[i]=s[i]; for(;i<(long)n;i++)d[i]=0; return i; }
void *kmemdup(const void *src, size_t len, unsigned gfp);
#endif
