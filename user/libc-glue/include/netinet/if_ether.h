/* netinet/if_ether.h — Ethernet header + ETHERTYPE_* (busybox udhcp). */
#ifndef _NANOS_NETINET_IF_ETHER_H
#define _NANOS_NETINET_IF_ETHER_H
#include <stdint.h>
#include <net/if_arp.h>
#define ETH_ALEN 6
#define ETHERTYPE_IP  0x0800
#define ETHERTYPE_ARP 0x0806
#define ETH_P_IP  0x0800
#define ETH_P_ARP 0x0806
struct ethhdr { uint8_t h_dest[6]; uint8_t h_source[6]; uint16_t h_proto; } __attribute__((packed));
struct ether_header { uint8_t ether_dhost[6]; uint8_t ether_shost[6]; uint16_t ether_type; } __attribute__((packed));
#endif
