/*
 * net/ethernet.h — Ethernet address/frame definitions. libuv's linux.c includes it for interface
 * enumeration (MAC addresses). NanOS supplies the standard glibc/BSD shapes; the MAC bytes come from
 * the kernel's interface data at runtime.
 */
#ifndef _NET_ETHERNET_H
#define _NET_ETHERNET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ETHER_ADDR_LEN  6
#define ETHER_TYPE_LEN  2
#define ETHER_HDR_LEN   14
#define ETH_ALEN        6

struct ether_addr {
	uint8_t ether_addr_octet[ETHER_ADDR_LEN];
};

struct ether_header {
	uint8_t  ether_dhost[ETHER_ADDR_LEN];
	uint8_t  ether_shost[ETHER_ADDR_LEN];
	uint16_t ether_type;
};

#define ETHERTYPE_IP   0x0800
#define ETHERTYPE_ARP  0x0806
#define ETHERTYPE_IPV6 0x86dd

#ifdef __cplusplus
}
#endif

#endif /* _NET_ETHERNET_H */
