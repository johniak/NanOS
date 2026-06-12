#include "NetBuf.h"

namespace kernel {

// A fixed pool of packet buffers. 128 * 2 KiB = 256 KiB of static BSS — enough for the e1000
// RX ring (a few dozen descriptors) plus in-flight TX/queued packets, with no heap churn on
// the hot path. A singly-linked free list threads through the pool via a side index array
// (we can't union the link into buf[] without risking aliasing the packet data).
namespace {
const int POOL_N = 128;
NetBuf g_pool[POOL_N];
int    g_freeNext[POOL_N];   // free-list links (index), -1 = end
int    g_freeHead = -2;      // -2 = not yet initialised
int    g_inUse = 0;

void initPool() {
	for (int i = 0; i < POOL_N; i++)
		g_freeNext[i] = i + 1;
	g_freeNext[POOL_N - 1] = -1;
	g_freeHead = 0;
	g_inUse = 0;
}
}  // namespace

NetBuf* netbufAlloc() {
	if (g_freeHead == -2)
		initPool();
	if (g_freeHead < 0)
		return 0;                       // exhausted: caller drops the packet (never blocks)
	int idx = g_freeHead;
	g_freeHead = g_freeNext[idx];
	g_inUse++;
	NetBuf* b = &g_pool[idx];
	b->data = 0; b->len = 0; b->dev = 0; b->protocol = 0; b->l3 = -1; b->l4 = -1;
	return b;
}

void netbufFree(NetBuf* b) {
	if (!b)
		return;
	int idx = (int) (b - g_pool);
	if (idx < 0 || idx >= POOL_N)
		return;                         // not one of ours (defensive)
	g_freeNext[idx] = g_freeHead;
	g_freeHead = idx;
	g_inUse--;
}

int netbufInUse()    { return g_inUse; }
int netbufCapacity() { return POOL_N; }

}  // namespace kernel
