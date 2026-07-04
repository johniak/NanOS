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

TEST_CASE("msiSetup uses single-vector MSI even when MSI-X is also present (single-queue NIC)") {
	memset(g_cfg, 0, sizeof g_cfg);
	memset(g_table, 0, sizeof g_table);
	g_cfg[0x06] = 0x10;                 // Status: capabilities-list present (bit4)
	g_cfg[0x34] = 0x40;                 // Capabilities pointer -> 0x40
	// cap @0x40: MSI (id 0x05), 32-bit (control bit7 clear), next -> 0x50
	g_cfg[0x40] = 0x05; g_cfg[0x41] = 0x50;
	// cap @0x50: MSI-X (id 0x11), next -> 0x00 — present but intentionally not used.
	g_cfg[0x50] = 0x11; g_cfg[0x51] = 0x00;
	g_cfg[0x54] = 0x04;                 // (MSI-X table offset/BIR — ignored)

	MsiEnv env;
	env.cfgRead = fcRead; env.cfgWrite = fcWrite; env.mapMmio = fMap;
	env.allocVector = fAlloc; env.lapicId = fId;

	MsiResult r = msiSetup(env, 0, 0, 0);
	CHECK(r.kind == MSI_KIND_MSI);
	CHECK(r.vector == 0x71);
	CHECK(r.capOff == 0x40);            // the MSI cap, not the MSI-X one
	uint32_t addr; memcpy(&addr, &g_cfg[0x44], 4);
	CHECK(addr == 0xFEE03000u);         // message address @cap+4 (lapicId 3 << 12)
	uint32_t data; memcpy(&data, &g_cfg[0x48], 4);
	CHECK(data == 0x71u);               // 32-bit: data @cap+8
	uint16_t mc; memcpy(&mc, &g_cfg[0x42], 2);
	CHECK((mc & 0x1) != 0);             // MSI enable
	CHECK(g_table[0] == 0u);            // MSI-X table left untouched
}

TEST_CASE("msiSetup programs single-vector MSI-X for an MSI-X-only device (QEMU virtio-pci)") {
	memset(g_cfg, 0, sizeof g_cfg);
	memset(g_table, 0, sizeof g_table);
	g_cfg[0x06] = 0x10;                        // capabilities-list present
	g_cfg[0x34] = 0x50;                        // cap ptr -> 0x50
	g_cfg[0x50] = 0x11; g_cfg[0x51] = 0x00;    // MSI-X cap (id 0x11), no next
	// Table BIR/Offset @cap+4 (0x54): BIR 0, offset 0.
	g_cfg[0x54] = 0x00; g_cfg[0x55] = 0x00; g_cfg[0x56] = 0x00; g_cfg[0x57] = 0x00;
	// BAR0 @0x10: a 32-bit memory BAR at 0xF0000000 (low 4 bits are flags -> masked off).
	g_cfg[0x10] = 0x00; g_cfg[0x11] = 0x00; g_cfg[0x12] = 0x00; g_cfg[0x13] = 0xF0;

	MsiEnv env;
	env.cfgRead = fcRead; env.cfgWrite = fcWrite; env.mapMmio = fMap;
	env.allocVector = fAlloc; env.lapicId = fId;

	MsiResult r = msiSetup(env, 0, 0, 0);
	CHECK(r.kind == MSI_KIND_MSIX);
	CHECK(r.vector == 0x71);
	CHECK(r.capOff == 0x50);
	// Table entry 0 programmed: addr(lapicId 3), addr-hi 0, data=vector, vector-control unmasked.
	CHECK(g_table[0] == 0xFEE03000u);
	CHECK(g_table[1] == 0u);
	CHECK(g_table[2] == 0x71u);
	CHECK((g_table[3] & 0x1u) == 0u);          // entry unmasked
	uint16_t mc; memcpy(&mc, &g_cfg[0x52], 2);
	CHECK((mc & 0x8000) != 0);                 // MSI-X Enable
	CHECK((mc & 0x4000) == 0);                 // Function Mask cleared
}

TEST_CASE("msiSetup returns MSI_NONE for an I/O-BAR MSI-X table (must be MMIO)") {
	memset(g_cfg, 0, sizeof g_cfg);
	g_cfg[0x06] = 0x10;
	g_cfg[0x34] = 0x50;
	g_cfg[0x50] = 0x11; g_cfg[0x51] = 0x00;
	g_cfg[0x54] = 0x00;                        // BIR 0, offset 0
	g_cfg[0x10] = 0x01;                        // BAR0 is an I/O BAR (bit0 set) -> reject
	MsiEnv env;
	env.cfgRead = fcRead; env.cfgWrite = fcWrite; env.mapMmio = fMap;
	env.allocVector = fAlloc; env.lapicId = fId;
	MsiResult r = msiSetup(env, 0, 0, 0);
	CHECK(r.kind == MSI_NONE);
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
