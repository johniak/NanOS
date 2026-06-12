#include "Route.h"
#include "NetDevice.h"

namespace kernel {

namespace {
const int ROUTE_N = 16;
Route g_routes[ROUTE_N];
}  // namespace

void routeAdd(uint32_t dest, uint32_t mask, uint32_t gw, NetDevice* dev, int metric) {
	dest &= mask;   // canonicalize: host bits in the network address are meaningless
	// Replace an existing dest/mask, else take a free slot.
	Route* slot = 0;
	for (int i = 0; i < ROUTE_N; i++) {
		if (g_routes[i].used && g_routes[i].dest == dest && g_routes[i].mask == mask) { slot = &g_routes[i]; break; }
		if (!slot && !g_routes[i].used) slot = &g_routes[i];
	}
	if (!slot) return;
	slot->dest = dest; slot->mask = mask; slot->gw = gw; slot->dev = dev; slot->metric = metric;
	slot->used = true;
}

void routeDel(uint32_t dest, uint32_t mask) {
	dest &= mask;
	for (int i = 0; i < ROUTE_N; i++)
		if (g_routes[i].used && g_routes[i].dest == dest && g_routes[i].mask == mask)
			g_routes[i].used = false;
}

bool routeLookup(uint32_t dst, NetDevice** dev, uint32_t* nexthop) {
	const Route* best = 0;
	for (int i = 0; i < ROUTE_N; i++) {
		const Route& r = g_routes[i];
		if (!r.used) continue;
		if ((dst & r.mask) != r.dest) continue;          // not on this prefix
		// Longest prefix wins; break ties by lower metric.
		if (!best || r.mask > best->mask || (r.mask == best->mask && r.metric < best->metric))
			best = &r;
	}
	if (!best) return false;
	if (dev) *dev = best->dev;
	if (nexthop) *nexthop = best->gw ? best->gw : dst;   // gateway, or the destination if on-link
	return true;
}

void routeAddDefault(NetDevice* dev, uint32_t gw) {
	if (!dev) return;
	uint32_t net = dev->ip & dev->netmask;
	routeAdd(net, dev->netmask, 0, dev, 0);              // on-link subnet (direct)
	routeAdd(0, 0, gw, dev, 0);                          // default via gw
}

int routeCount() {
	int n = 0;
	for (int i = 0; i < ROUTE_N; i++) if (g_routes[i].used) n++;
	return n;
}
const Route* routeByIndex(int i) {
	int n = 0;
	for (int k = 0; k < ROUTE_N; k++)
		if (g_routes[k].used && n++ == i) return &g_routes[k];
	return 0;
}
void routeReset() { for (int i = 0; i < ROUTE_N; i++) g_routes[i].used = false; }

}  // namespace kernel
