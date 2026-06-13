/*
 * dhcpcfg.c — the udhcpc action helper, installed as /nanos/config/udhcpc.script. busybox udhcpc
 * exec()s this with argv[1] = the action ("bound"/"renew"/"deconfig"/...) and the lease in the
 * environment (interface, ip, subnet, router, dns). On a lease it applies the address + netmask
 * (SIOCSIFADDR/SIOCSIFNETMASK), installs the default route (SIOCADDRT) and rewrites
 * /etc/resolv.conf — the Linux udhcpc.script flow, done with ioctls since NanOS ships no ifconfig.
 *
 * It is a compiled .nxe (not a shell script) so udhcpc's plain exec() runs it directly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/route.h>

static void set_ifaddr(int s, const char* iface, unsigned long cmd, const char* dotted) {
	struct ifreq ifr;
	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
	struct sockaddr_in* sin = (struct sockaddr_in*) &ifr.ifr_addr;
	sin->sin_family = AF_INET;
	inet_aton(dotted, &sin->sin_addr);
	ioctl(s, cmd, &ifr);
}

int main(int argc, char** argv) {
	const char* action = argc > 1 ? argv[1] : "";
	const char* iface  = getenv("interface"); if (!iface) iface = "eth0";
	const char* ip     = getenv("ip");
	const char* mask   = getenv("subnet");
	const char* router = getenv("router");
	const char* dns    = getenv("dns");

	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) return 1;

	if ((!strcmp(action, "bound") || !strcmp(action, "renew")) && ip) {
		set_ifaddr(s, iface, SIOCSIFADDR, ip);
		if (mask) set_ifaddr(s, iface, SIOCSIFNETMASK, mask);
		if (router && *router) {
			struct rtentry rt;
			memset(&rt, 0, sizeof rt);
			struct sockaddr_in* g = (struct sockaddr_in*) &rt.rt_gateway;
			g->sin_family = AF_INET; inet_aton(router, &g->sin_addr);
			((struct sockaddr_in*) &rt.rt_dst)->sin_family = AF_INET;        /* 0.0.0.0 */
			((struct sockaddr_in*) &rt.rt_genmask)->sin_family = AF_INET;    /* 0.0.0.0 */
			rt.rt_flags = RTF_UP | RTF_GATEWAY;
			ioctl(s, SIOCADDRT, &rt);
		}
		if (dns && *dns) {
			FILE* f = fopen("/etc/resolv.conf", "w");
			if (f) {
				char buf[256]; strncpy(buf, dns, sizeof buf - 1); buf[sizeof buf - 1] = 0;
				for (char* p = strtok(buf, " \t"); p; p = strtok(0, " \t"))
					fprintf(f, "nameserver %s\n", p);
				fclose(f);
			}
		}
		printf("udhcpc: %s = %s/%s gw %s\n", iface, ip, mask ? mask : "?", router ? router : "-");
	}
	close(s);
	return 0;
}
