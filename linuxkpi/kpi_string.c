/*
 * linuxkpi/kpi_string.c — string/memory helpers for the kext build of the LinuxKPI shim.
 *
 * The kext runtime (kext/kext_rt.cpp) already provides memset/memcpy (compiler-emitted),
 * so they are intentionally NOT redefined here. Everything else Linux source and the shim
 * reference is provided. This file is KEXT-ONLY (not in the host TEST_MODULES): the host
 * doctest harness gets these from libc, so compiling it there would multiply-define them.
 */
#include <linux/types.h>

size_t strlen(const char *s) {
	const char *p = s;
	while (*p) p++;
	return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max) {
	size_t n = 0;
	while (n < max && s[n]) n++;
	return n;
}

int strcmp(const char *a, const char *b) {
	while (*a && (*a == *b)) { a++; b++; }
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
	for (size_t i = 0; i < n; i++) {
		unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
		if (ca != cb) return (int)ca - (int)cb;
		if (!ca) break;
	}
	return 0;
}

char *strcpy(char *d, const char *s) {
	char *r = d;
	while ((*d++ = *s++)) ;
	return r;
}

char *strncpy(char *d, const char *s, size_t n) {
	size_t i = 0;
	for (; i < n && s[i]; i++) d[i] = s[i];
	for (; i < n; i++) d[i] = '\0';
	return d;
}

size_t strlcpy(char *d, const char *s, size_t size) {
	size_t sl = strlen(s);
	if (size) {
		size_t n = (sl < size - 1) ? sl : size - 1;
		for (size_t i = 0; i < n; i++) d[i] = s[i];
		d[n] = '\0';
	}
	return sl;
}

/* Linux strscpy: copy with truncation; returns copied length or -E2BIG (-7). */
long strscpy(char *d, const char *s, size_t size) {
	size_t i = 0;
	if (!size) return -7;
	for (; i < size - 1 && s[i]; i++) d[i] = s[i];
	d[i] = '\0';
	return s[i] ? -7 : (long)i;
}

char *strcat(char *d, const char *s) {
	char *r = d;
	while (*d) d++;
	while ((*d++ = *s++)) ;
	return r;
}

char *strchr(const char *s, int c) {
	for (; *s; s++)
		if (*s == (char)c) return (char *)s;
	return (c == 0) ? (char *)s : 0;
}

char *strrchr(const char *s, int c) {
	const char *last = 0;
	for (; *s; s++)
		if (*s == (char)c) last = s;
	if (c == 0) return (char *)s;
	return (char *)last;
}

char *strstr(const char *h, const char *n) {
	if (!*n) return (char *)h;
	for (; *h; h++) {
		const char *a = h, *b = n;
		while (*a && *b && (*a == *b)) { a++; b++; }
		if (!*b) return (char *)h;
	}
	return 0;
}

int memcmp(const void *a, const void *b, size_t n) {
	const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
	for (size_t i = 0; i < n; i++)
		if (x[i] != y[i]) return (int)x[i] - (int)y[i];
	return 0;
}

void *memchr(const void *s, int c, size_t n) {
	const unsigned char *p = (const unsigned char *)s;
	for (size_t i = 0; i < n; i++)
		if (p[i] == (unsigned char)c) return (void *)(p + i);
	return 0;
}

void *memmove(void *d, const void *s, size_t n) {
	unsigned char *dd = (unsigned char *)d;
	const unsigned char *ss = (const unsigned char *)s;
	if (dd == ss || n == 0) return d;
	if (dd < ss)
		for (size_t i = 0; i < n; i++) dd[i] = ss[i];
	else
		for (size_t i = n; i > 0; i--) dd[i - 1] = ss[i - 1];
	return d;
}
