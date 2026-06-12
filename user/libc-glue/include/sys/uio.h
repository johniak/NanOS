/*
 * sys/uio.h — NanOS SDK scatter/gather I/O surface. POSIX places `struct iovec` here (not in
 * <sys/socket.h>), which gnulib and many Linux apps rely on; defining it here lets gnulib's
 * <sys/uio.h> replacement include_next ours instead of redefining the struct.
 */
#ifndef _SYS_UIO_H
#define _SYS_UIO_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct iovec {
	void*  iov_base;
	size_t iov_len;
};

ssize_t readv(int fd, const struct iovec* iov, int iovcnt);
ssize_t writev(int fd, const struct iovec* iov, int iovcnt);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_UIO_H */
