/*
 * Route.h — the IPv4 routing table: longest-prefix match + default route, the way Linux picks
 * a next hop. Each entry is dest/mask via gw (0 = on-link) out a device. routeLookup returns
 * the device and the next-hop IP to ARP for. All addresses host byte order.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct NetDevice;

struct Route {
	uint32_t   dest;     // network address (host order)
	uint32_t   mask;     // netmask (host order); 0 = default route
	uint32_t   gw;       // gateway, or 0 for on-link (next hop = the destination itself)
	NetDevice* dev;
	int        metric;
	bool       used;
};

// Add a route (dest/mask via gw out dev). A duplicate dest/mask is replaced.
void routeAdd(uint32_t dest, uint32_t mask, uint32_t gw, NetDevice* dev, int metric);
void routeDel(uint32_t dest, uint32_t mask);

// Resolve dst to an egress device + next-hop IP (the gateway for off-link, else dst itself).
// Longest-prefix wins; the default route (mask 0) is the fallback. Returns false if no route.
bool routeLookup(uint32_t dst, NetDevice** dev, uint32_t* nexthop);

// Convenience: add the on-link subnet route + a default route for a freshly-configured device.
void routeAddDefault(NetDevice* dev, uint32_t gw);

int  routeCount();
const Route* routeByIndex(int i);
void routeReset();

}  // namespace kernel
