/*
 * arpa/inet.h — address conversion (inet_pton/ntop/aton/addr/ntoa) + byte order. Overrides
 * picolibc's minimal arpa/inet.h (which only had the htons macros) with the full surface Linux
 * network apps expect. Implementations live in libc.ndl (net.c).
 */
#ifndef _ARPA_INET_H
#define _ARPA_INET_H

#include <netinet/in.h>   /* in_addr/in_addr_t + htons/htonl/ntohs/ntohl */

#ifdef __cplusplus
extern "C" {
#endif

in_addr_t   inet_addr(const char* cp);
int         inet_aton(const char* cp, struct in_addr* inp);
char*       inet_ntoa(struct in_addr in);
int         inet_pton(int af, const char* src, void* dst);
const char* inet_ntop(int af, const void* src, char* dst, socklen_t size);

#ifdef __cplusplus
}
#endif

#endif /* _ARPA_INET_H */
