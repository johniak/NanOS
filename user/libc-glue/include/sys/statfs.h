/* sys/statfs.h — filesystem stats. Stub for ports (busybox libbb.h includes it); decls only. */
#ifndef _NANOS_SYS_STATFS_H
#define _NANOS_SYS_STATFS_H
#include <sys/types.h>
typedef struct { int __val[2]; } __nanos_fsid_t;
struct statfs {
	long f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree;
	__nanos_fsid_t f_fsid;
	long f_namelen, f_frsize, f_flags, f_spare[4];
};
#ifdef __cplusplus
extern "C" {
#endif
int statfs(const char* path, struct statfs* buf);
int fstatfs(int fd, struct statfs* buf);
#ifdef __cplusplus
}
#endif
#endif
