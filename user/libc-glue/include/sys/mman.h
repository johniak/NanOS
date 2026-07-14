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
/* MAP_NORESERVE: don't reserve swap. NanOS has no swap and backs mappings eagerly, so it is a no-op
 * hint (V8 uses it when reserving large address-space regions it won't fully commit). */
#define MAP_NORESERVE 0x4000
#define MAP_POPULATE  0x8000   /* prefault pages — NanOS already backs eagerly, so it's a no-op hint */
#define MAP_STACK     0x20000  /* advisory; no-op on NanOS */

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
#define MADV_FREE       8
#define MADV_DONTFORK   10   /* child must not inherit this range — advisory no-op on NanOS */
#define MADV_DONTDUMP   16

/* mremap(2): resize/move a mapping. MREMAP_MAYMOVE lets the kernel relocate it. NanOS has no
 * demand-paging remap, so mremap always fails (MAP_FAILED/ENOMEM) and callers (V8) fall back to
 * allocate-copy-free. The flags/decl exist so the source compiles. */
#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED   2
void* mremap(void* old_addr, size_t old_size, size_t new_size, int flags, ...);

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
int   munmap(void* addr, size_t length);
int   mprotect(void* addr, size_t length, int prot);
int   msync(void* addr, size_t length, int flags);
int   madvise(void* addr, size_t length, int advice);
int   mincore(void* addr, size_t length, unsigned char* vec);

/* Memory locking. NanOS has no swap and eagerly backs every mapping, so pages are ALWAYS resident —
 * mlock/mlockall are correct success no-ops (the pages the caller wants pinned already are). Used by
 * OpenSSL's secure heap (Node's bundled OpenSSL) and by libuv. */
#define MCL_CURRENT 1
#define MCL_FUTURE  2
int   mlock(const void* addr, size_t len);
int   munlock(const void* addr, size_t len);
int   mlockall(int flags);
int   munlockall(void);

/* memfd_create(2): an anonymous, frame-backed fd whose mmap(MAP_SHARED) is real cross-process shared
 * memory. Implemented as a real SYS_memfd_create wrapper in libc-glue/syscalls.c. */
int   memfd_create(const char* name, unsigned int flags);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_MMAN_H */
