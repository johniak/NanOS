/* libnanos — the stable named API a NanOS program calls. The wrappers trap into
 * the kernel via int 0x80 (see libnanos.c). No imports: the program is fully
 * self-contained (the import-by-name/.ndl path is future work). */
#ifndef LIBNANOS_H
#define LIBNANOS_H

#define NX_NIMPORTS 0

int write(int fd, const void* buf, unsigned n);
int read(int fd, void* buf, unsigned n);
int open(const char* path, int flags);
int close(int fd);
void exit(int code);

#endif
