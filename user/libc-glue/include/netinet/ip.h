/*
 * netinet/ip.h — IPv4 header (glibc i686 layout). Used by ping's libicmp to build/parse IP.
 */
#ifndef _NETINET_IP_H
#define _NETINET_IP_H

#include <stdint.h>
#include <netinet/in.h>

struct timestamp {
	uint8_t len;
	uint8_t ptr;
	unsigned int flags:4;
	unsigned int overflow:4;
	uint32_t data[9];
};

struct iphdr {
	unsigned int ihl:4;
	unsigned int version:4;
	uint8_t  tos;
	uint16_t tot_len;
	uint16_t id;
	uint16_t frag_off;
	uint8_t  ttl;
	uint8_t  protocol;
	uint16_t check;
	uint32_t saddr;
	uint32_t daddr;
	/* options start here */
};

/* BSD-style struct ip (little-endian member order for i686). */
struct ip {
	unsigned int ip_hl:4;   /* header length */
	unsigned int ip_v:4;    /* version */
	uint8_t  ip_tos;        /* type of service */
	unsigned short ip_len;  /* total length */
	unsigned short ip_id;   /* identification */
	unsigned short ip_off;  /* fragment offset field */
#define IP_RF      0x8000   /* reserved fragment flag */
#define IP_DF      0x4000   /* dont fragment flag */
#define IP_MF      0x2000   /* more fragments flag */
#define IP_OFFMASK 0x1fff   /* mask for fragmenting bits */
	uint8_t  ip_ttl;        /* time to live */
	uint8_t  ip_p;          /* protocol */
	unsigned short ip_sum;  /* checksum */
	struct in_addr ip_src, ip_dst; /* source and dest address */
};

#define IP_MAXPACKET  65535   /* maximum packet size */
#define IPVERSION     4

/* Type of service (ip_tos). */
#define IPTOS_TOS_MASK     0x1E
#define IPTOS_TOS(tos)     ((tos) & IPTOS_TOS_MASK)
#define IPTOS_LOWDELAY     0x10
#define IPTOS_THROUGHPUT   0x08
#define IPTOS_RELIABILITY  0x04
#define IPTOS_LOWCOST      0x02
#define IPTOS_MINCOST      IPTOS_LOWCOST

/* IP precedence (ip_tos high 3 bits). */
#define IPTOS_PREC_MASK            0xe0
#define IPTOS_PREC(tos)            ((tos) & IPTOS_PREC_MASK)
#define IPTOS_PREC_NETCONTROL      0xe0
#define IPTOS_PREC_INTERNETCONTROL 0xc0
#define IPTOS_PREC_CRITIC_ECP      0xa0
#define IPTOS_PREC_FLASHOVERRIDE   0x80
#define IPTOS_PREC_FLASH           0x60
#define IPTOS_PREC_IMMEDIATE       0x40
#define IPTOS_PREC_PRIORITY        0x20
#define IPTOS_PREC_ROUTINE         0x00

/* Time to live. */
#define MAXTTL       255
#define IPDEFTTL     64
#define IPFRAGTTL    60
#define IPTTLDEC     1
#define IP_MSS       576

/* IP options (ip_p == option type byte). */
#define IPOPT_COPY      0x80
#define IPOPT_CLASS_MASK 0x60
#define IPOPT_NUMBER_MASK 0x1f
#define IPOPT_COPIED(o)  ((o) & IPOPT_COPY)
#define IPOPT_CLASS(o)   ((o) & IPOPT_CLASS_MASK)
#define IPOPT_NUMBER(o)  ((o) & IPOPT_NUMBER_MASK)

#define IPOPT_CONTROL    0x00
#define IPOPT_RESERVED1  0x20
#define IPOPT_MEASUREMENT 0x40
#define IPOPT_RESERVED2  0x60

#define IPOPT_EOL        0   /* end of option list */
#define IPOPT_END        IPOPT_EOL
#define IPOPT_NOP        1   /* no operation */
#define IPOPT_NOOP       IPOPT_NOP
#define IPOPT_RR         7   /* record packet route */
#define IPOPT_TS         68  /* timestamp */
#define IPOPT_TIMESTAMP  IPOPT_TS
#define IPOPT_SECURITY   130 /* provide s,c,h,tcc */
#define IPOPT_SEC        IPOPT_SECURITY
#define IPOPT_LSRR       131 /* loose source route */
#define IPOPT_SATID      136 /* satnet id */
#define IPOPT_SID        IPOPT_SATID
#define IPOPT_SSRR       137 /* strict source route */
#define IPOPT_RA         148 /* router alert */

/* Offsets within an option. */
#define IPOPT_OPTVAL     0   /* option ID */
#define IPOPT_OLEN       1   /* option length */
#define IPOPT_OFFSET     2   /* offset within option */
#define IPOPT_MINOFF     4   /* min value of above */
#define MAX_IPOPTLEN     40

/* Timestamp option (IPOPT_TS) flag values + overflow byte position. */
#define IPOPT_POS_OV_FLG 3   /* position of overflow/flag byte */
#define IPOPT_TS_TSONLY    0 /* timestamps only */
#define IPOPT_TS_TSANDADDR 1 /* timestamps and addresses */
#define IPOPT_TS_PRESPEC   3 /* specified modules only */

#endif /* _NETINET_IP_H */
