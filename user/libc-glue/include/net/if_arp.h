/* net/if_arp.h — ARP wire + ioctl structs (busybox udhcp arpping). */
#ifndef _NANOS_NET_IF_ARP_H
#define _NANOS_NET_IF_ARP_H
#include <stdint.h>
#include <sys/socket.h>
#define ARPHRD_ETHER 1
#define ARPOP_REQUEST 1
#define ARPOP_REPLY   2
struct arphdr { uint16_t ar_hrd, ar_pro; uint8_t ar_hln, ar_pln; uint16_t ar_op; };
struct arpreq {
	struct sockaddr arp_pa; struct sockaddr arp_ha; int arp_flags;
	struct sockaddr arp_netmask; char arp_dev[16];
};
#define ATF_COM 0x02
#endif
