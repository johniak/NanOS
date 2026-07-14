/*
 * ifaddrs.h — getifaddrs(3) interface-address enumeration. c-ares (and node's os.networkInterfaces)
 * use it to list local interfaces. NanOS's libc-glue currently returns an EMPTY list (getifaddrs
 * succeeds with *ifap == NULL); callers fall back to other source-address selection, and DNS works
 * via /etc/hosts + the configured resolver. A real enumeration (loopback + eth0 via SIOCGIFADDR) is a
 * documented follow-up.
 */
#ifndef _IFADDRS_H
#define _IFADDRS_H

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ifaddrs {
	struct ifaddrs  *ifa_next;
	char            *ifa_name;
	unsigned int     ifa_flags;
	struct sockaddr *ifa_addr;
	struct sockaddr *ifa_netmask;
	union {
		struct sockaddr *ifu_broadaddr;
		struct sockaddr *ifu_dstaddr;
	} ifa_ifu;
	void            *ifa_data;
};
#define ifa_broadaddr ifa_ifu.ifu_broadaddr
#define ifa_dstaddr   ifa_ifu.ifu_dstaddr

int  getifaddrs(struct ifaddrs **ifap);
void freeifaddrs(struct ifaddrs *ifa);

#ifdef __cplusplus
}
#endif

#endif /* _IFADDRS_H */
