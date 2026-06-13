/*
 * ifname.c — the <net/if.h> name<->index helpers (if_nametoindex / if_indextoname / if_nameindex
 * / if_freenameindex). picolibc declares them but does not implement them for NanOS. They are
 * built over the SIOCGIFINDEX ioctl the kernel already serves (kernel/Syscall.cpp netIoctl).
 * inetutils' ifconfig (generic backend) enumerates interfaces through if_nameindex().
 *
 * NanOS exposes a fixed, small set of interfaces (lo + eth0); if_nameindex probes that candidate
 * list and reports the ones that actually exist (SIOCGIFINDEX succeeds). No netlink, no
 * SIOCGIFCONF parsing — a candidate probe is exact for a system whose interfaces are known.
 */
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* The interfaces NanOS may have, in the order ifconfig should list them. */
static const char* const NX_IFACES[] = { "lo", "eth0", 0 };

unsigned int if_nametoindex(const char* ifname) {
	if (!ifname) return 0;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return 0;
	struct ifreq ifr;
	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	int r = ioctl(fd, SIOCGIFINDEX, &ifr);
	close(fd);
	return r < 0 ? 0 : (unsigned int) ifr.ifr_ifindex;
}

char* if_indextoname(unsigned int ifindex, char* ifname) {
	if (!ifname) return 0;
	for (int i = 0; NX_IFACES[i]; i++) {
		unsigned idx = if_nametoindex(NX_IFACES[i]);
		if (idx && idx == ifindex) {
			strncpy(ifname, NX_IFACES[i], IFNAMSIZ - 1);
			ifname[IFNAMSIZ - 1] = 0;
			return ifname;
		}
	}
	return 0;
}

struct if_nameindex* if_nameindex(void) {
	/* Count the candidates that exist, then build a heap array terminated by a zero entry
	 * (if_index == 0 && if_name == NULL), as POSIX specifies. */
	struct if_nameindex* arr =
		(struct if_nameindex*) calloc(sizeof(NX_IFACES) / sizeof(NX_IFACES[0]) + 1,
		                              sizeof(struct if_nameindex));
	if (!arr) return 0;
	int n = 0;
	for (int i = 0; NX_IFACES[i]; i++) {
		unsigned idx = if_nametoindex(NX_IFACES[i]);
		if (!idx) continue;
		arr[n].if_index = idx;
		arr[n].if_name = strdup(NX_IFACES[i]);
		n++;
	}
	/* arr[n] stays zeroed = the terminating entry. */
	return arr;
}

void if_freenameindex(struct if_nameindex* ptr) {
	if (!ptr) return;
	for (int i = 0; ptr[i].if_index != 0 || ptr[i].if_name; i++)
		free(ptr[i].if_name);
	free(ptr);
}
