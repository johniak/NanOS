/*
 * nx-node-data.c — libc DATA symbols that Node references directly (not through the __imp_ dllimport
 * shim, which only covers stdio/errno/environ). On the NanOS .nxe model a program can't RIP-relative-
 * reference a libc.ndl data export, so these are defined LOCALLY and linked straight into node.nxe.
 * Compiled WITHOUT nx-dllimport.h so the names are real definitions, not __imp_ redirects.
 */
#include <netinet/in.h>

/* The IPv6 wildcard (::) and loopback (::1) addresses (glibc exports these as data). */
const struct in6_addr in6addr_any = { { 0 } };
const struct in6_addr in6addr_loopback = {
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }
};
