/*
 * sys/socket.h — NanOS SDK networking surface (Linux i686-compatible layouts/constants), so
 * precompiled-style Linux network apps build unchanged. picolibc ships no networking; this and
 * the sibling net headers supply it, backed by the socketcall ABI wrappers in libc.ndl (net.c).
 */
#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

#include <sys/types.h>
#include <stdint.h>
#include <sys/uio.h>   /* struct iovec (POSIX home), shared with gnulib's <sys/uio.h> */

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

/* Maximum listen() backlog (sudo's log-server client references it). */
#ifndef SOMAXCONN
#define SOMAXCONN      128
#endif

/* Socket types. The high bits (Linux) OR in behaviour flags. */
#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_RDM       4
#define SOCK_PACKET    10
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
#define MSG_CMSG_CLOEXEC 0x40000000   /* set O_CLOEXEC on fds received via SCM_RIGHTS (libuv) */

/* shutdown(2). */
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

/* SO_LINGER argument (linger-on-close). The kernel ignores the timeout, but the struct must exist
 * for setsockopt(SO_LINGER) callers (e.g. OpenSSL's s_time/s_client) to compile. */
struct linger {
	int l_onoff;    /* linger active */
	int l_linger;   /* how long to linger (seconds) */
};

struct sockaddr {
	sa_family_t sa_family;
	char        sa_data[14];
};

struct sockaddr_storage {
	sa_family_t ss_family;
	char        __ss_pad[128 - sizeof(sa_family_t)];
};

/* struct iovec now lives in <sys/uio.h> (included above). */

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

/* Ancillary-data accessors (ports like busybox udhcpc walk recvmsg control buffers). NanOS recvmsg
 * returns no control messages today, so CMSG_FIRSTHDR yields NULL and callers take the no-cmsg path. */
#define CMSG_ALIGN(len)   (((len) + sizeof(size_t) - 1) & (size_t) ~(sizeof(size_t) - 1))
#define CMSG_DATA(cmsg)   ((unsigned char*)((struct cmsghdr*)(cmsg) + 1))
#define CMSG_LEN(len)     (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#define CMSG_SPACE(len)   (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(len))
#define CMSG_FIRSTHDR(m)  ((size_t)(m)->msg_controllen >= sizeof(struct cmsghdr) \
                            ? (struct cmsghdr*)(m)->msg_control : (struct cmsghdr*)0)
#define CMSG_NXTHDR(m, c) ((char*)(c) + CMSG_ALIGN((c)->cmsg_len) + sizeof(struct cmsghdr) \
                            > (char*)(m)->msg_control + (m)->msg_controllen ? (struct cmsghdr*)0 \
                            : (struct cmsghdr*)((char*)(c) + CMSG_ALIGN((c)->cmsg_len)))
#define SCM_RIGHTS 0x01

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

/* Multi-message send/recv (Linux batch syscalls). libuv's UDP path uses them; NanOS has no batch
 * syscall, so libc-glue loops over sendmsg/recvmsg. struct timespec* timeout is accepted, ignored. */
struct mmsghdr {
	struct msghdr msg_hdr;
	unsigned int  msg_len;
};
struct timespec;
int recvmmsg(int fd, struct mmsghdr* msgvec, unsigned int vlen, int flags, struct timespec* timeout);
int sendmmsg(int fd, struct mmsghdr* msgvec, unsigned int vlen, int flags);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SOCKET_H */
