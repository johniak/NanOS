/* linux/route.h — struct rtentry for SIOCADDRT/SIOCDELRT (DHCP default-route install). Layout
 * matches kernel/Syscall.cpp netIoctl: rt_dst@4, rt_gateway@20, rt_genmask@36, rt_flags@52. */
#ifndef _NANOS_LINUX_ROUTE_H
#define _NANOS_LINUX_ROUTE_H
#include <sys/socket.h>
struct rtentry {
	unsigned long  rt_pad1;
	struct sockaddr rt_dst;
	struct sockaddr rt_gateway;
	struct sockaddr rt_genmask;
	unsigned short rt_flags;
	short          rt_pad2;
	unsigned long  rt_pad3;
	unsigned char  rt_tos;
	unsigned char  rt_class;
	short          rt_pad4[3];
	short          rt_metric;
	char*          rt_dev;
	unsigned long  rt_mtu;
	unsigned long  rt_window;
	unsigned short rt_irtt;
};
#define RTF_UP      0x0001
#define RTF_GATEWAY 0x0002
#define RTF_HOST    0x0004
#endif
