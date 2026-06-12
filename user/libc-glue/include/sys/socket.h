/*
 * sys/socket.h — NanOS SDK networking surface (Linux i686-compatible layouts/constants), so
 * precompiled-style Linux network apps build unchanged. picolibc ships no networking; this and
 * the sibling net headers supply it, backed by the socketcall ABI wrappers in libc.ndl (net.c).
 */
#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

#include <sys/types.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t socklen_t;
typedef unsigned short sa_family_t;

/* Address families (Linux values). */
#define AF_UNSPEC   0
#define AF_UNIX     1
#define AF_LOCAL    1
#define AF_INET     2
#define AF_INET6    10
#define AF_PACKET   17
#define PF_UNSPEC   AF_UNSPEC
#define PF_UNIX     AF_UNIX
#define PF_INET     AF_INET
#define PF_INET6    AF_INET6
#define PF_PACKET   AF_PACKET

/* Socket types. The high bits (Linux) OR in behaviour flags. */
#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_SEQPACKET 5
#define SOCK_CLOEXEC   02000000   /* = 0x80000 */
#define SOCK_NONBLOCK  04000      /* = 0x800 */

/* setsockopt levels + SOL_SOCKET option names (Linux). */
#define SOL_SOCKET   1
#define SO_DEBUG     1
#define SO_REUSEADDR 2
#define SO_TYPE      3
#define SO_ERROR     4
#define SO_DONTROUTE 5
#define SO_BROADCAST 6
#define SO_SNDBUF    7
#define SO_RCVBUF    8
#define SO_KEEPALIVE 9
#define SO_OOBINLINE 10
#define SO_LINGER    13
#define SO_REUSEPORT 15
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21

/* send/recv flags. */
#define MSG_OOB       0x01
#define MSG_PEEK      0x02
#define MSG_DONTROUTE 0x04
#define MSG_CTRUNC    0x08
#define MSG_TRUNC     0x20
#define MSG_DONTWAIT  0x40
#define MSG_EOR       0x80
#define MSG_WAITALL   0x100
#define MSG_NOSIGNAL  0x4000

/* shutdown(2). */
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

struct sockaddr {
	sa_family_t sa_family;
	char        sa_data[14];
};

struct sockaddr_storage {
	sa_family_t ss_family;
	char        __ss_pad[128 - sizeof(sa_family_t)];
};

struct iovec {
	void*  iov_base;
	size_t iov_len;
};

struct msghdr {
	void*         msg_name;
	socklen_t     msg_namelen;
	struct iovec* msg_iov;
	size_t        msg_iovlen;
	void*         msg_control;
	size_t        msg_controllen;
	int           msg_flags;
};

struct cmsghdr {
	size_t cmsg_len;
	int    cmsg_level;
	int    cmsg_type;
};

int socket(int domain, int type, int protocol);
int socketpair(int domain, int type, int protocol, int sv[2]);
int bind(int fd, const struct sockaddr* addr, socklen_t len);
int connect(int fd, const struct sockaddr* addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr* addr, socklen_t* len);
int accept4(int fd, struct sockaddr* addr, socklen_t* len, int flags);
int getsockname(int fd, struct sockaddr* addr, socklen_t* len);
int getpeername(int fd, struct sockaddr* addr, socklen_t* len);
int getsockopt(int fd, int level, int name, void* val, socklen_t* len);
int setsockopt(int fd, int level, int name, const void* val, socklen_t len);
int shutdown(int fd, int how);
ssize_t send(int fd, const void* buf, size_t len, int flags);
ssize_t recv(int fd, void* buf, size_t len, int flags);
ssize_t sendto(int fd, const void* buf, size_t len, int flags, const struct sockaddr* addr, socklen_t alen);
ssize_t recvfrom(int fd, void* buf, size_t len, int flags, struct sockaddr* addr, socklen_t* alen);
ssize_t sendmsg(int fd, const struct msghdr* msg, int flags);
ssize_t recvmsg(int fd, struct msghdr* msg, int flags);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SOCKET_H */
