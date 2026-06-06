/* libnanos — the stable named API a NanOS program imports. The loader binds
 * each name to a kernel export, so one binary runs on any NanOS build. */
#ifndef LIBNANOS_H
#define LIBNANOS_H

#define NX_NIMPORTS 5

int write(int fd, const void* buf, unsigned n);
int read(int fd, void* buf, unsigned n);
int open(const char* path, int flags);
int close(int fd);
void exit(int code);

#endif
