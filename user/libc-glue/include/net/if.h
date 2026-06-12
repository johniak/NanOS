/*
 * net/if.h — network interface name/index surface (Linux i686 layout). NanOS userland resolves
 * interfaces by name through the socket ioctl ABI; this header gives apps the standard structs
 * and if_nametoindex/if_indextoname declarations.
 */
#ifndef _NET_IF_H
#define _NET_IF_H

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IF_NAMESIZE 16
#define IFNAMSIZ    IF_NAMESIZE

/* Interface flags (Linux values). */
#define IFF_UP          0x1
#define IFF_BROADCAST   0x2
#define IFF_DEBUG       0x4
#define IFF_LOOPBACK    0x8
#define IFF_POINTOPOINT 0x10
#define IFF_RUNNING     0x40
#define IFF_NOARP       0x80
#define IFF_PROMISC     0x100
#define IFF_MULTICAST   0x1000

struct if_nameindex {
	unsigned int if_index;
	char*        if_name;
};

struct ifreq {
	char ifr_name[IFNAMSIZ];
	union {
		struct sockaddr ifru_addr;
		struct sockaddr ifru_dstaddr;
		struct sockaddr ifru_broadaddr;
		struct sockaddr ifru_netmask;
		struct sockaddr ifru_hwaddr;
		short           ifru_flags;
		int             ifru_ivalue;
		int             ifru_mtu;
		char            ifru_slave[IFNAMSIZ];
		char            ifru_newname[IFNAMSIZ];
		char*           ifru_data;
	} ifr_ifru;
};
#define ifr_addr      ifr_ifru.ifru_addr
#define ifr_dstaddr   ifr_ifru.ifru_dstaddr
#define ifr_broadaddr ifr_ifru.ifru_broadaddr
#define ifr_netmask   ifr_ifru.ifru_netmask
#define ifr_hwaddr    ifr_ifru.ifru_hwaddr
#define ifr_flags     ifr_ifru.ifru_flags
#define ifr_ifindex   ifr_ifru.ifru_ivalue
#define ifr_mtu       ifr_ifru.ifru_mtu
#define ifr_data      ifr_ifru.ifru_data

struct ifconf {
	int len;
	union {
		char*          ifcu_buf;
		struct ifreq*  ifcu_req;
	} ifc_ifcu;
};
#define ifc_buf ifc_ifcu.ifcu_buf
#define ifc_req ifc_ifcu.ifcu_req

unsigned int        if_nametoindex(const char* ifname);
char*               if_indextoname(unsigned int ifindex, char* ifname);
struct if_nameindex* if_nameindex(void);
void                if_freenameindex(struct if_nameindex* ptr);

#ifdef __cplusplus
}
#endif

#endif /* _NET_IF_H */
