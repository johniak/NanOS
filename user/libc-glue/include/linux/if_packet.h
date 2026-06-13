/* linux/if_packet.h — AF_PACKET address + pkttypes (busybox udhcpc). Matches the kernel ABI in
 * net/Packet.cpp / kernel sockaddr_ll marshalling. */
#ifndef _NANOS_LINUX_IF_PACKET_H
#define _NANOS_LINUX_IF_PACKET_H
#include <stdint.h>
struct sockaddr_ll {
	unsigned short sll_family;
	unsigned short sll_protocol;
	int            sll_ifindex;
	unsigned short sll_hatype;
	unsigned char  sll_pkttype;
	unsigned char  sll_halen;
	unsigned char  sll_addr[8];
};
#define PACKET_HOST      0
#define PACKET_BROADCAST 1
#define PACKET_MULTICAST 2
#define PACKET_OTHERHOST 3
#define PACKET_OUTGOING  4
#define SOL_PACKET       263
#define PACKET_AUXDATA   8
struct tpacket_auxdata {
	unsigned int tp_status, tp_len, tp_snaplen;
	unsigned short tp_mac, tp_net;
	unsigned short tp_vlan_tci, tp_vlan_tpid;
};
#define TP_STATUS_CSUMNOTREADY (1 << 3)
#define TP_STATUS_CSUM_VALID   (1 << 7)
#endif
