/* sys/statvfs.h — POSIX filesystem stats. Stub for ports (git's compat/posix.h includes it
 * unconditionally); decls only. NanOS has no statvfs syscall, but ports that merely include
 * the header (and never call it, like git) just need the struct + prototypes to exist. */
#ifndef _NANOS_SYS_STATVFS_H
#define _NANOS_SYS_STATVFS_H
#include <sys/types.h>   /* fsblkcnt_t / fsfilcnt_t */

struct statvfs {
	unsigned long f_bsize;   /* file system block size */
	unsigned long f_frsize;  /* fragment size */
	fsblkcnt_t    f_blocks;  /* size of fs in f_frsize units */
	fsblkcnt_t    f_bfree;   /* # free blocks */
	fsblkcnt_t    f_bavail;  /* # free blocks for unprivileged users */
	fsfilcnt_t    f_files;   /* # inodes */
	fsfilcnt_t    f_ffree;   /* # free inodes */
	fsfilcnt_t    f_favail;  /* # free inodes for unprivileged users */
	unsigned long f_fsid;    /* file system ID */
	unsigned long f_flag;    /* mount flags */
	unsigned long f_namemax; /* maximum filename length */
};

/* f_flag bits */
#define ST_RDONLY 0x0001
#define ST_NOSUID 0x0002

#ifdef __cplusplus
extern "C" {
#endif
int statvfs(const char* path, struct statvfs* buf);
int fstatvfs(int fd, struct statvfs* buf);
#ifdef __cplusplus
}
#endif
#endif
