/*
 * netinet/in_systm.h — historical BSD network types used by <netinet/ip.h> and ping's libicmp.
 */
#ifndef _NETINET_IN_SYSTM_H
#define _NETINET_IN_SYSTM_H

#include <stdint.h>

typedef uint16_t n_short;   /* short as received from the net */
typedef uint32_t n_long;    /* long as received from the net */
typedef uint32_t n_time;    /* ms since 00:00 GMT, byte rev */

#endif /* _NETINET_IN_SYSTM_H */
