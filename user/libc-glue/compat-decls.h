/*
 * compat-decls.h — declarations picolibc omits for the i686-elf target but that
 * the verbatim sbase sources reference. Force-included via -include so the vendored
 * programs stay unmodified. The definitions live in user/libc-glue/syscalls.c.
 */
#ifndef NX_COMPAT_DECLS_H
#define NX_COMPAT_DECLS_H

struct stat;
int lstat(const char* path, struct stat* buf);

/* Userland cwd resolver (user/libc-glue/cwd.c): expand a path to absolute. */
void nx_resolve(const char* path, char* out);

#endif /* NX_COMPAT_DECLS_H */
