/*
 * sys/mman.h — memory mapping (Linux i686 constants). NanOS's mmap eagerly backs mappings with
 * allocated pages (anonymous, device, or file-backed); munmap is a no-op (no unmap syscall yet).
 * Enough for tools that mmap-then-read-then-exit (wget).
 */
#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS

#define MAP_FAILED ((void*) -1)

/* msync / madvise flags (accepted, mostly advisory/no-op on NanOS). */
#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4
#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
int   munmap(void* addr, size_t length);
int   mprotect(void* addr, size_t length, int prot);
int   msync(void* addr, size_t length, int flags);
int   madvise(void* addr, size_t length, int advice);
int   mincore(void* addr, size_t length, unsigned char* vec);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_MMAN_H */
