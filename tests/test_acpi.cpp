#include "doctest.h"
#include "Acpi.h"
#include <string.h>
#include <vector>
using namespace kernel;

// A flat fake "physical memory" the parser reads through a callback.
static std::vector<unsigned char> g_mem;
static bool fakeRead(uint64_t pa, void* dst, uint32_t len) {
	if (pa + len > g_mem.size()) return false;
	memcpy(dst, &g_mem[pa], len);
	return true;
}
static uint8_t sum8(const unsigned char* p, int n) { uint8_t s = 0; for (int i = 0; i < n; i++) s += p[i]; return s; }

TEST_CASE("MADT yields the enabled CPU LAPIC ids") {
	g_mem.assign(4096, 0);
	auto put32 = [&](int off, uint32_t v){ memcpy(&g_mem[off], &v, 4); };
	// --- MADT @ 0x200: 44-byte header + N Processor-Local-APIC entries (8 bytes each).
	int m = 0x200;
	memcpy(&g_mem[m], "APIC", 4);
	int e = m + 44;
	auto lapic = [&](uint8_t apicId, bool enabled){
		g_mem[e] = 0; g_mem[e + 1] = 8; g_mem[e + 2] = apicId; g_mem[e + 3] = apicId;
		put32(e + 4, enabled ? 1 : 0); e += 8;
	};
	lapic(0, true); lapic(1, true); lapic(2, false); lapic(3, true);   // 3 enabled, 1 disabled
	put32(m + 4, (uint32_t)(e - m));                                   // MADT length
	g_mem[m + 9] = (uint8_t)(0u - sum8(&g_mem[m], e - m) + g_mem[m + 9]);   // checksum -> 0
	// --- RSDT @ 0x100: 36-byte header + one 32-bit pointer to the MADT.
	int r = 0x100; memcpy(&g_mem[r], "RSDT", 4); put32(r + 4, 40); put32(r + 36, m);
	g_mem[r + 9] = (uint8_t)(0u - sum8(&g_mem[r], 40) + g_mem[r + 9]);
	// --- RSDP @ 0x40: "RSD PTR ", checksum, RSDT ptr @16, revision 0 (use RSDT).
	int p = 0x40; memcpy(&g_mem[p], "RSD PTR ", 8); put32(p + 16, r);
	g_mem[p + 8] = (uint8_t)(0u - sum8(&g_mem[p], 20) + g_mem[p + 8]);

	Acpi acpi(fakeRead);
	CHECK(acpi.parse(p));                 // start from the RSDP physical address
	CHECK(acpi.cpuCount() == 3);          // only enabled CPUs
	CHECK(acpi.lapicId(0) == 0);
	CHECK(acpi.lapicId(1) == 1);
	CHECK(acpi.lapicId(2) == 3);
}

TEST_CASE("bad RSDP checksum is rejected") {
	g_mem.assign(256, 0);
	memcpy(&g_mem[0x40], "RSD PTR ", 8);  // signature present but no valid checksum
	Acpi acpi(fakeRead);
	CHECK_FALSE(acpi.parse(0x40));
}

TEST_CASE("wrong RSDP signature is rejected") {
	g_mem.assign(256, 0);
	memcpy(&g_mem[0x40], "NOTRSDP!", 8);
	Acpi acpi(fakeRead);
	CHECK_FALSE(acpi.parse(0x40));
}

TEST_CASE("RSDP rev>=2 walks the XSDT (64-bit pointers)") {
	g_mem.assign(8192, 0);
	auto put32 = [&](int off, uint32_t v){ memcpy(&g_mem[off], &v, 4); };
	auto put64 = [&](int off, uint64_t v){ memcpy(&g_mem[off], &v, 8); };
	// MADT @ 0x400: two enabled CPUs (ids 7, 9).
	int m = 0x400; memcpy(&g_mem[m], "APIC", 4);
	int e = m + 44;
	auto lapic = [&](uint8_t id){ g_mem[e]=0; g_mem[e+1]=8; g_mem[e+2]=id; g_mem[e+3]=id; put32(e+4,1); e+=8; };
	lapic(7); lapic(9);
	put32(m + 4, (uint32_t)(e - m));
	g_mem[m + 9] = (uint8_t)(0u - sum8(&g_mem[m], e - m) + g_mem[m + 9]);
	// XSDT @ 0x300: 36-byte header + one 64-bit pointer to the MADT.
	int x = 0x300; memcpy(&g_mem[x], "XSDT", 4); put32(x + 4, 44); put64(x + 36, m);
	g_mem[x + 9] = (uint8_t)(0u - sum8(&g_mem[x], 44) + g_mem[x + 9]);
	// Extended RSDP @ 0x40: revision 2, XSDT ptr @24, 20-byte + 36-byte checksums both 0.
	int p = 0x40; memcpy(&g_mem[p], "RSD PTR ", 8); g_mem[p + 15] = 2; put64(p + 24, x);
	g_mem[p + 8]  = (uint8_t)(0u - sum8(&g_mem[p], 20) + g_mem[p + 8]);   // base checksum
	g_mem[p + 32] = (uint8_t)(0u - sum8(&g_mem[p], 36) + g_mem[p + 32]);  // extended checksum

	Acpi acpi(fakeRead);
	CHECK(acpi.parse(p));
	CHECK(acpi.cpuCount() == 2);
	CHECK(acpi.lapicId(0) == 7);
	CHECK(acpi.lapicId(1) == 9);
}

TEST_CASE("RSDT with no MADT yields no CPUs") {
	g_mem.assign(1024, 0);
	auto put32 = [&](int off, uint32_t v){ memcpy(&g_mem[off], &v, 4); };
	// RSDT @ 0x100 pointing at a non-APIC table (FACP) -> parse fails (no MADT found).
	int f = 0x200; memcpy(&g_mem[f], "FACP", 4); put32(f + 4, 36);
	g_mem[f + 9] = (uint8_t)(0u - sum8(&g_mem[f], 36) + g_mem[f + 9]);
	int r = 0x100; memcpy(&g_mem[r], "RSDT", 4); put32(r + 4, 40); put32(r + 36, f);
	g_mem[r + 9] = (uint8_t)(0u - sum8(&g_mem[r], 40) + g_mem[r + 9]);
	int p = 0x40; memcpy(&g_mem[p], "RSD PTR ", 8); put32(p + 16, r);
	g_mem[p + 8] = (uint8_t)(0u - sum8(&g_mem[p], 20) + g_mem[p + 8]);

	Acpi acpi(fakeRead);
	CHECK_FALSE(acpi.parse(p));
	CHECK(acpi.cpuCount() == 0);
}
