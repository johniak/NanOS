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
void *memmove(void*, const void*, size_t);
size_t strnlen(const char*, size_t);
#ifndef NANOS_HOST_TEST
/* On the host test path glibc's <string.h> already declares these (const-correct, with C++
 * overloads); redeclaring them (memchr/strchr/strrchr/strstr) conflicts. The kext/freestanding
 * path needs the declarations, so emit them only there. */
void *memchr(const void*, int, size_t);
char *strchr(const char*, int);
char *strrchr(const char*, int);
char *strstr(const char*, const char*);
#endif
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
#endif

#ifndef _LKPI_STRING_X3
#define _LKPI_STRING_X3
static inline char *strnchr(const char *s, size_t n, int c){ for(size_t i=0;i<n&&s[i];i++) if(s[i]==(char)c) return (char*)&s[i]; return 0; }
static inline size_t str_has_prefix(const char *s, const char *pfx){ size_t i=0; for(;pfx[i];i++) if(s[i]!=pfx[i]) return 0; return i; }
long simple_strtol(const char*, char**, unsigned);
unsigned long simple_strtoul(const char*, char**, unsigned);
#endif

#ifndef _LKPI_STRING_X4
#define _LKPI_STRING_X4
static inline void strtomem_pad(void *dest, const char *src, char pad){ (void)pad; size_t i=0; char *d=(char*)dest; while(src[i]){d[i]=src[i];i++;} }
static inline int mem_is_zero(const void *s, size_t n){ const unsigned char *p=(const unsigned char*)s; for(size_t i=0;i<n;i++) if(p[i]) return 0; return 1; }
#endif

#ifndef _LKPI_STRING_X5
#define _LKPI_STRING_X5
/* word-granular memset variants (i915 fills PTE/ring pages with them) */
static inline void *memset32(u32 *s, u32 v, size_t count){ for(size_t i=0;i<count;i++) s[i]=v; return s; }
static inline void *memset64(u64 *s, u64 v, size_t count){ for(size_t i=0;i<count;i++) s[i]=v; return s; }
/* first byte != v within n (Linux memchr_inv), or NULL if all bytes equal v */
static inline void *memchr_inv(const void *p, int v, size_t n){ const unsigned char *s=(const unsigned char*)p; for(size_t i=0;i<n;i++) if(s[i]!=(unsigned char)v) return (void*)&s[i]; return 0; }
/* strsep: split *sp at any delimiter, advancing *sp past it (returns the token, NUL-terminated).
 * kext-only: glibc's <string.h> already declares strsep (non-static) on the host test path. */
#ifndef NANOS_HOST_TEST
static inline char *strsep(char **sp, const char *delim){ char *s=*sp, *t; if(!s) return 0; for(t=s;*t;t++){ const char *d; for(d=delim;*d;d++) if(*t==*d){ *t=0; *sp=t+1; return s; } } *sp=0; return s; }
#endif
#endif

#ifndef _LKPI_STRING_X6
#define _LKPI_STRING_X6
/* strim: strip leading+trailing whitespace in place, return the trimmed start. */
static inline char *strim(char *s){
	char *e; if(!s) return s;
	while(*s==' '||*s=='\t'||*s=='\n'||*s=='\r') s++;
	if(!*s) return s;
	e = s; while(*e) e++;
	for(e--; e>s && (*e==' '||*e=='\t'||*e=='\n'||*e=='\r'); e--) ;
	e[1]=0; return s;
}
static inline char *skip_spaces(const char *s){ while(*s==' '||*s=='\t') s++; return (char*)s; }
/* sscanf/vsscanf: declared here (real impl in the C runtime / kpi); i915 uses them only on the
 * debugfs write path, off the display bring-up path. */
int sscanf(const char *buf, const char *fmt, ...);
int vsscanf(const char *buf, const char *fmt, __builtin_va_list ap);
#endif
