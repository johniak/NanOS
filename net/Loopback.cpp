#include "Loopback.h"
#include "NetDevice.h"
#include "NetBuf.h"
#include "Net.h"
#include <string.h>

namespace kernel {

static NetDevice g_lo;

// lo transmit = loop the fully-built frame straight back to RX. The skb already carries an
// Ethernet header (the stack builds it uniformly for every device); eth_rx will demux it. We
// take ownership and hand it to netifRx, so the softirq thread delivers it — exactly like
// Linux loopback going through netif_rx (never synchronous, never in IRQ).
static int loTx(NetDevice* dev, NetBuf* skb) {
	skb->dev = dev;            // ingress device is lo
	skb->l3 = skb->l4 = -1;    // force a fresh parse on the RX side
	return netifRx(skb);
}

NetDevice* loopbackCreate() {
	memset(&g_lo, 0, sizeof(g_lo));
	g_lo.name[0] = 'l'; g_lo.name[1] = 'o';
	g_lo.mtu = 1500;           // real lo is 65536, but our NetBuf is 2 KiB; 1500 is plenty here
	g_lo.flags = NETIF_UP | NETIF_RUNNING | NETIF_LOOPBACK;
	g_lo.ip = ipv4(127, 0, 0, 1);
	g_lo.netmask = ipv4(255, 0, 0, 0);
	g_lo.broadcast = 0;
	g_lo.tx = loTx;
	netRegister(&g_lo);
	return &g_lo;
}

}  // namespace kernel
