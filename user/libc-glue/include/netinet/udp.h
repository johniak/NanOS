/* netinet/udp.h — UDP header (busybox udhcp common.h). Both glibc + BSD field names via a union. */
#ifndef _NANOS_NETINET_UDP_H
#define _NANOS_NETINET_UDP_H
#include <stdint.h>
struct udphdr {
	union {
		struct { uint16_t source, dest, len, check; };
		struct { uint16_t uh_sport, uh_dport, uh_ulen, uh_sum; };
	};
};
#endif
