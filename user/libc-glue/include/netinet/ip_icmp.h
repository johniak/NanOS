/*
 * netinet/ip_icmp.h — ICMP header + message struct (glibc i686 layout). This is the core header
 * ping's libicmp builds echo requests and decodes replies/errors against.
 */
#ifndef _NETINET_IP_ICMP_H
#define _NETINET_IP_ICMP_H

#include <stdint.h>
#include <netinet/in.h>
#include <netinet/ip.h>

struct icmphdr {
	uint8_t  type;
	uint8_t  code;
	uint16_t checksum;
	union {
		struct {
			uint16_t id;
			uint16_t sequence;
		} echo;
		uint32_t gateway;
		struct {
			uint16_t __glibc_reserved;
			uint16_t mtu;
		} frag;
	} un;
};

struct icmp_ra_addr {
	uint32_t ira_addr;
	uint32_t ira_preference;
};

struct icmp {
	uint8_t  icmp_type;    /* type of message, see below */
	uint8_t  icmp_code;    /* type sub code */
	uint16_t icmp_cksum;   /* ones complement checksum of struct */
	union {
		unsigned char ih_pptr;       /* ICMP_PARAMPROB */
		struct in_addr ih_gwaddr;    /* gateway address */
		struct ih_idseq {            /* echo datagram */
			uint16_t icd_id;
			uint16_t icd_seq;
		} ih_idseq;
		uint32_t ih_void;
		/* ICMP_UNREACH_NEEDFRAG -- Path MTU Discovery (RFC1191) */
		struct ih_pmtu {
			uint16_t ipm_void;
			uint16_t ipm_nextmtu;
		} ih_pmtu;
		struct ih_rtradv {
			uint8_t  irt_num_addrs;
			uint8_t  irt_wpa;
			uint16_t irt_lifetime;
		} ih_rtradv;
	} icmp_hun;
#define icmp_pptr      icmp_hun.ih_pptr
#define icmp_gwaddr    icmp_hun.ih_gwaddr
#define icmp_id        icmp_hun.ih_idseq.icd_id
#define icmp_seq       icmp_hun.ih_idseq.icd_seq
#define icmp_void      icmp_hun.ih_void
#define icmp_pmvoid    icmp_hun.ih_pmtu.ipm_void
#define icmp_nextmtu   icmp_hun.ih_pmtu.ipm_nextmtu
#define icmp_num_addrs icmp_hun.ih_rtradv.irt_num_addrs
#define icmp_wpa       icmp_hun.ih_rtradv.irt_wpa
#define icmp_lifetime  icmp_hun.ih_rtradv.irt_lifetime
	union {
		struct {                     /* ICMP_TSTAMP */
			uint32_t its_otime;
			uint32_t its_rtime;
			uint32_t its_ttime;
		} id_ts;
		struct {
			struct ip idi_ip;
			/* options and then 64 bits of data */
		} id_ip;
		struct icmp_ra_addr id_radv;
		uint32_t id_mask;
		uint8_t  id_data[1];
	} icmp_dun;
#define icmp_otime  icmp_dun.id_ts.its_otime
#define icmp_rtime  icmp_dun.id_ts.its_rtime
#define icmp_ttime  icmp_dun.id_ts.its_ttime
#define icmp_ip     icmp_dun.id_ip.idi_ip
#define icmp_radv   icmp_dun.id_radv
#define icmp_mask   icmp_dun.id_mask
#define icmp_data   icmp_dun.id_data
};

/* Lengths. */
#define ICMP_MINLEN     8     /* abs minimum */
#define ICMP_TSLEN      (8 + 3 * sizeof (uint32_t)) /* timestamp */
#define ICMP_MASKLEN    12    /* address mask */
#define ICMP_ADVLENMIN  (8 + sizeof (struct ip) + 8) /* min */
#define ICMP_ADVLEN(p)  (8 + ((p)->icmp_ip.ip_hl << 2) + 8)

/* Definition of type and code field values. */
#define ICMP_ECHOREPLY       0    /* echo reply */
#define ICMP_DEST_UNREACH    3    /* dest unreachable, codes: */
#define   ICMP_NET_UNREACH   0    /* network unreachable */
#define   ICMP_HOST_UNREACH  1    /* host unreachable */
#define   ICMP_PROT_UNREACH  2    /* protocol unreachable */
#define   ICMP_PORT_UNREACH  3    /* port unreachable */
#define   ICMP_FRAG_NEEDED   4    /* frag needed and DF set */
#define   ICMP_SR_FAILED     5    /* source route failed */
#define   ICMP_NET_UNKNOWN   6    /* unknown net */
#define   ICMP_HOST_UNKNOWN  7    /* unknown host */
#define   ICMP_HOST_ISOLATED 8    /* src host isolated */
#define   ICMP_NET_UNR_TOS   11   /* net unreachable for TOS */
#define   ICMP_HOST_UNR_TOS  12   /* host unreachable for TOS */
#define   ICMP_PKT_FILTERED  13   /* packet filtered */
#define   ICMP_PREC_VIOLATION 14  /* precedence violation */
#define   ICMP_PREC_CUTOFF   15   /* precedence cut off */
#define ICMP_SOURCE_QUENCH   4    /* packet lost, slow down */
#define ICMP_REDIRECT        5    /* shorter route, codes: */
#define   ICMP_REDIR_NET     0    /* for network */
#define   ICMP_REDIR_HOST    1    /* for host */
#define   ICMP_REDIR_NETTOS  2    /* for tos and net */
#define   ICMP_REDIR_HOSTTOS 3    /* for tos and host */
#define ICMP_ECHO            8    /* echo service */
#define ICMP_ROUTERADV       9    /* router advertisement */
#define   ICMP_ROUTERADV_NORMAL 0
#define   ICMP_ROUTERADV_NOROUTE 16
#define ICMP_ROUTERDISCOVERY 10   /* router solicitation */
#define ICMP_TIME_EXCEEDED   11   /* time exceeded, code: */
#define   ICMP_EXC_TTL       0    /* ttl==0 in transit */
#define   ICMP_EXC_FRAGTIME  1    /* ttl==0 in reass */
#define ICMP_PARAMETERPROB   12   /* ip header bad */
#define ICMP_TIMESTAMP       13   /* timestamp request */
#define ICMP_TIMESTAMPREPLY  14   /* timestamp reply */
#define ICMP_INFO_REQUEST    15   /* information request */
#define ICMP_INFO_REPLY      16   /* information reply */
#define ICMP_ADDRESS         17   /* address mask request */
#define ICMP_ADDRESSREPLY    18   /* address mask reply */
#define ICMP_MAXTYPE         18

#endif /* _NETINET_IP_ICMP_H */
