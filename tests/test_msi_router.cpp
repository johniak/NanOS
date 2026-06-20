#include "doctest.h"
#include "MsiRouter.h"
#include <cstring>
using namespace kernel;

static uint8_t g_cfg[256];
static uint32_t fcRead(uint8_t, uint8_t, uint8_t, uint8_t off) {
	uint32_t v; memcpy(&v, &g_cfg[off & 0xFC], 4); return v;
}
static void fcWrite(uint8_t, uint8_t, uint8_t, uint8_t off, uint32_t v) {
	memcpy(&g_cfg[off & 0xFC], &v, 4);
}
static int     fAlloc() { return 0x71; }
static uint8_t fId()    { return 3; }
static uint32_t g_table[4];
static void*   fMap(uint32_t, uint32_t) { return g_table; }

TEST_CASE("msiSetup prefers MSI-X, allocates a vector and enables the cap") {
	memset(g_cfg, 0, sizeof g_cfg);
	memset(g_table, 0, sizeof g_table);
	g_cfg[0x06] = 0x10;                 // Status: capabilities-list present (bit4)
	g_cfg[0x34] = 0x40;                 // Capabilities pointer -> 0x40
	// cap @0x40: MSI (id 0x05), next -> 0x50
	g_cfg[0x40] = 0x05; g_cfg[0x41] = 0x50;
	// cap @0x50: MSI-X (id 0x11), next -> 0x00
	g_cfg[0x50] = 0x11; g_cfg[0x51] = 0x00;
	// table offset/BIR at 0x54: BIR = 4, offset = 0
	g_cfg[0x54] = 0x04;
	// BAR4 @0x20: a memory BAR (base irrelevant — fMap returns g_table)
	g_cfg[0x22] = 0xF0; g_cfg[0x23] = 0xFE;

	MsiEnv env;
	env.cfgRead = fcRead; env.cfgWrite = fcWrite; env.mapMmio = fMap;
	env.allocVector = fAlloc; env.lapicId = fId;

	MsiResult r = msiSetup(env, 0, 0, 0);
	CHECK(r.kind == MSI_KIND_MSIX);
	CHECK(r.vector == 0x71);
	CHECK(r.capOff == 0x50);
	// MSI-X table entry 0: addr = 0xFEE03000 (lapicId 3 << 12), data = 0x71, unmasked.
	CHECK(g_table[0] == 0xFEE03000u);
	CHECK(g_table[2] == 0x71u);
	CHECK((g_table[3] & 1u) == 0u);
	// MSI-X enable bit (message-control bit15) set in config space.
	uint16_t mc; memcpy(&mc, &g_cfg[0x52], 2);
	CHECK((mc & 0x8000) != 0);
}

TEST_CASE("msiSetup falls back to MSI when only MSI is present (32-bit)") {
	memset(g_cfg, 0, sizeof g_cfg);
	g_cfg[0x06] = 0x10;
	g_cfg[0x34] = 0x40;
	g_cfg[0x40] = 0x05; g_cfg[0x41] = 0x00;   // MSI only, 32-bit (message-control bit7 clear)

	MsiEnv env;
	env.cfgRead = fcRead; env.cfgWrite = fcWrite; env.mapMmio = fMap;
	env.allocVector = fAlloc; env.lapicId = fId;

	MsiResult r = msiSetup(env, 0, 0, 0);
	CHECK(r.kind == MSI_KIND_MSI);
	CHECK(r.vector == 0x71);
	uint32_t addr; memcpy(&addr, &g_cfg[0x44], 4);
	CHECK(addr == 0xFEE03000u);                // message address @cap+4
	uint32_t data; memcpy(&data, &g_cfg[0x48], 4);
	CHECK(data == 0x71u);                      // 32-bit: data @cap+8
	uint16_t mc; memcpy(&mc, &g_cfg[0x42], 2);
	CHECK((mc & 0x1) != 0);                    // MSI enable
}

TEST_CASE("msiSetup returns MSI_NONE when there is no capability list") {
	memset(g_cfg, 0, sizeof g_cfg);            // Status bit4 clear
	MsiEnv env;
	env.cfgRead = fcRead; env.cfgWrite = fcWrite; env.mapMmio = fMap;
	env.allocVector = fAlloc; env.lapicId = fId;
	MsiResult r = msiSetup(env, 0, 0, 0);
	CHECK(r.kind == MSI_NONE);
	CHECK(r.vector == -1);
}
