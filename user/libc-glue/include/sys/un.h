/*
 * sys/un.h — Unix-domain socket address (Linux layout). NanOS has no AF_UNIX transport yet, but
 * apps reference struct sockaddr_un at compile time; the declaration is enough.
 */
#ifndef _SYS_UN_H
#define _SYS_UN_H

#include <sys/socket.h>

#define UNIX_PATH_MAX 108

struct sockaddr_un {
	sa_family_t sun_family;
	char        sun_path[UNIX_PATH_MAX];
};

#endif /* _SYS_UN_H */
