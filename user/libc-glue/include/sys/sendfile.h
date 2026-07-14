/*
 * sys/sendfile.h — sendfile(2) zero-copy-style file transfer. NanOS has no kernel sendfile, so
 * libc-glue implements it in userland (a read/write loop); libuv's uv_fs_sendfile uses it.
 */
#ifndef _SYS_SENDFILE_H
#define _SYS_SENDFILE_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

ssize_t sendfile(int out_fd, int in_fd, off_t* offset, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SENDFILE_H */
