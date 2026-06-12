#include "doctest.h"
#include "NetBuf.h"
#include "NetDevice.h"
#include "Loopback.h"
#include "Net.h"
#include <cstdint>
#include <cstring>

using namespace kernel;

// ---- NetBuf -----------------------------------------------------------------

TEST_CASE("NetBuf: reserve/put/push/pull mirror sk_buff geometry") {
	NetBuf* b = netbufAlloc();
	REQUIRE(b);
	b->reserve(64);                 // 64 bytes of headroom for stack-built headers
	CHECK(b->headroom() == 64);
	CHECK(b->len == 0);

	unsigned char* payload = b->put(20);   // append 20-byte payload at the tail
	for (int i = 0; i < 20; i++) payload[i] = (unsigned char) (0xA0 + i);
	CHECK(b->len == 20);

	unsigned char* hdr = b->push(8);        // prepend an 8-byte header (e.g. UDP)
	CHECK(b->len == 28);
	CHECK(b->head() == hdr);
	CHECK(b->headroom() == 56);
	hdr[0] = 0xDE;

	// Data is contiguous: header then payload.
	CHECK(b->head()[0] == 0xDE);
	CHECK(b->head()[8] == 0xA0);

	b->pull(8);                             // consume the header on "RX"
	CHECK(b->len == 20);
	CHECK(b->head()[0] == 0xA0);

	netbufFree(b);
}

TEST_CASE("NetBuf pool: exhaustion returns null, free replenishes") {
	// Drain the pool, confirm the next alloc fails, then free one and succeed.
	int cap = netbufCapacity();
	NetBuf** all = new NetBuf*[cap];
	int got = 0;
	for (int i = 0; i < cap; i++) { all[i] = netbufAlloc(); if (all[i]) got++; }
	CHECK(got == cap);
	CHECK(netbufAlloc() == nullptr);        // exhausted -> drop, never block
	netbufFree(all[0]);
	NetBuf* again = netbufAlloc();
	CHECK(again != nullptr);
	netbufFree(again);
	for (int i = 1; i < cap; i++) netbufFree(all[i]);
	delete[] all;
	CHECK(netbufInUse() == 0);
}

// ---- registry ---------------------------------------------------------------

static NetDevice makeDev(const char* name, uint32_t flags) {
	NetDevice d; std::memset(&d, 0, sizeof(d));
	std::strncpy(d.name, name, 15);
	d.mtu = 1500; d.flags = flags;
	return d;
}

TEST_CASE("registry: register/lookup/iterate, primary skips loopback") {
	netReset();
	static NetDevice lo = makeDev("lo", NETIF_UP | NETIF_RUNNING | NETIF_LOOPBACK);
	static NetDevice eth = makeDev("eth0", NETIF_UP | NETIF_RUNNING | NETIF_BROADCAST);
	CHECK(netRegister(&lo));
	CHECK(netRegister(&eth));
	CHECK(netCount() == 2);
	CHECK(netByName("eth0") == &eth);
	CHECK(netByName("lo") == &lo);
	CHECK(netByName("nope") == nullptr);
	CHECK(netPrimary() == &eth);            // first non-loopback UP device
	netReset();
}

// ---- RX bottom-half ---------------------------------------------------------

static int g_seen;
static int g_lastLen;
static NetDevice* g_lastDev;
static void captureInput(NetBuf* skb) {
	g_seen++;
	g_lastLen = skb->len;
	g_lastDev = skb->dev;
	netbufFree(skb);                        // handler owns the skb
}

TEST_CASE("netifRx enqueues; netRxProcess drains to the input handler") {
	netReset();
	g_seen = 0; g_lastLen = 0; g_lastDev = nullptr;
	netSetInputHandler(captureInput);
	static NetDevice eth = makeDev("eth0", NETIF_UP);

	NetBuf* b = netbufAlloc();
	b->reserve(0); b->put(42); b->dev = &eth;
	CHECK(netRxPending() == false);
	CHECK(netifRx(b) == 0);
	CHECK(netRxPending() == true);          // queued, not yet delivered (deferred to softirq)
	CHECK(g_seen == 0);

	int n = netRxProcess();
	CHECK(n == 1);
	CHECK(g_seen == 1);
	CHECK(g_lastLen == 42);
	CHECK(g_lastDev == &eth);
	CHECK(eth.rxPackets == 1);
	CHECK(eth.rxBytes == 42);
	CHECK(netbufInUse() == 0);
	netReset();
}

TEST_CASE("backlog overflow drops + bumps rxDropped, never blocks") {
	netReset();
	netSetInputHandler(captureInput);
	static NetDevice eth = makeDev("eth0", NETIF_UP);
	// Pool (128) > backlog (64), so enqueue past the backlog depth must DROP (rxDropped++),
	// not wedge — and the pool must not be exhausted first.
	int queued = 0, dropped = 0;
	for (int i = 0; i < 100; i++) {
		NetBuf* b = netbufAlloc();
		REQUIRE(b);                         // pool is larger than backlog: alloc must succeed
		b->reserve(0); b->put(4); b->dev = &eth;
		if (netifRx(b) == 0) queued++; else dropped++;
	}
	CHECK(queued == 64);                    // exactly the backlog depth
	CHECK(dropped == 36);
	CHECK(eth.rxDropped == 36);
	netReset();                             // drains the 64 queued buffers (covers reset-drain)
	CHECK(netbufInUse() == 0);
}

// ---- transmit paths ---------------------------------------------------------

static int g_txCalls;
static int dummyTxOk(NetDevice*, NetBuf* skb)  { g_txCalls++; netbufFree(skb); return 0; }   // owns skb
static int dummyTxErr(NetDevice*, NetBuf* skb) { g_txCalls++; netbufFree(skb); return -5; }  // owns skb (frees even on error)

TEST_CASE("netTransmit: success bumps tx stats; tx owns the skb") {
	netReset();
	g_txCalls = 0;
	static NetDevice eth = makeDev("eth0", NETIF_UP);
	eth.tx = dummyTxOk;
	NetBuf* b = netbufAlloc(); b->reserve(0); b->put(60);
	CHECK(netTransmit(&eth, b) == 0);
	CHECK(g_txCalls == 1);
	CHECK(eth.txPackets == 1);
	CHECK(eth.txBytes == 60);
	CHECK(netbufInUse() == 0);
	netReset();
}

TEST_CASE("netTransmit: driver error bumps txErrors and frees the skb (no leak)") {
	netReset();
	g_txCalls = 0;
	static NetDevice eth = makeDev("eth0", NETIF_UP);
	eth.tx = dummyTxErr;
	NetBuf* b = netbufAlloc(); b->reserve(0); b->put(60);
	CHECK(netTransmit(&eth, b) == -5);
	CHECK(eth.txErrors == 1);
	CHECK(netbufInUse() == 0);              // netTransmit freed it on error
	netReset();
}

TEST_CASE("netTransmit: no tx fn / null args are handled without crashing or leaking") {
	netReset();
	static NetDevice eth = makeDev("eth0", NETIF_UP);
	eth.tx = nullptr;
	NetBuf* b = netbufAlloc(); b->put(10);
	CHECK(netTransmit(&eth, b) == -1);      // no tx -> txDropped, skb freed
	CHECK(eth.txDropped == 1);
	CHECK(netTransmit(nullptr, nullptr) == -1);
	CHECK(netbufInUse() == 0);
	netReset();
}

TEST_CASE("netByIndex iterates all registered devices") {
	netReset();
	static NetDevice a = makeDev("lo",   NETIF_UP | NETIF_LOOPBACK);
	static NetDevice b = makeDev("eth0", NETIF_UP);
	netRegister(&a); netRegister(&b);
	CHECK(netByIndex(0) == &a);
	CHECK(netByIndex(1) == &b);
	CHECK(netByIndex(2) == nullptr);
	CHECK(netByIndex(-1) == nullptr);
	netReset();
	CHECK(netCount() == 0);
}

// IRQ-guard hooks: the kernel installs interrupt save/restore around the backlog critical
// section. Verify they're invoked in balanced pairs and don't change behaviour.
static int g_guardDepth, g_guardMax, g_saves;
static unsigned long testSave()  { g_saves++; g_guardDepth++; if (g_guardDepth > g_guardMax) g_guardMax = g_guardDepth; return 0x55; }
static void testRestore(unsigned long f) { CHECK(f == 0x55); g_guardDepth--; }

TEST_CASE("IRQ-guard hooks wrap the backlog critical section in balanced pairs") {
	netReset();
	g_guardDepth = g_guardMax = g_saves = 0;
	netSetIrqGuard(testSave, testRestore);
	netSetInputHandler(captureInput);
	static NetDevice eth = makeDev("eth0", NETIF_UP);
	NetBuf* b = netbufAlloc(); b->put(8); b->dev = &eth;
	netifRx(b);
	netRxProcess();
	CHECK(g_saves > 0);
	CHECK(g_guardDepth == 0);                // every save paired with a restore
	CHECK(g_guardMax == 1);                  // never nested
	netReset();
}

// ---- loopback ---------------------------------------------------------------

TEST_CASE("loopback: tx re-injects into RX and the handler sees it on lo") {
	netReset();
	g_seen = 0; g_lastDev = nullptr;
	netSetInputHandler(captureInput);
	NetDevice* lo = loopbackCreate();
	REQUIRE(lo);
	CHECK((lo->flags & NETIF_LOOPBACK) != 0);
	CHECK(lo->ip == ipv4(127, 0, 0, 1));

	NetBuf* b = netbufAlloc();
	b->reserve(16); b->put(100);            // a 100-byte "frame"
	int rc = netTransmit(lo, b);
	CHECK(rc == 0);
	CHECK(lo->txPackets == 1);
	CHECK(g_seen == 0);                     // not delivered synchronously
	netRxProcess();
	CHECK(g_seen == 1);
	CHECK(g_lastDev == lo);
	CHECK(g_lastLen == 100);
	CHECK(netbufInUse() == 0);
	netReset();
}
