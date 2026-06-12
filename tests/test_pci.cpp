#include "doctest.h"
#include "Pci.h"
#include <cstdint>
#include <cstring>
#include <vector>

using namespace kernel;

// ----------------------------------------------------------------------------
// Mock PCI config space. Each function owns 256 bytes (64 dwords). BARs emulate
// real hardware sizing: the low log2(size) address bits are read-only-zero, so
// writing 0xFFFFFFFF and reading back yields the size mask. We model that with a
// per-BAR writable-address mask + fixed flag bits, exactly as silicon decodes it.
// ----------------------------------------------------------------------------
struct MockFunc {
	bool     present = false;
	uint32_t cfg[64] = {0};
	uint32_t barMask[6] = {0};   // writable address bits (0 = slot is plain config, not a sized BAR)
	uint32_t barFlags[6] = {0};  // hardwired low flag bits for that BAR
};

static MockFunc g_space[256][32][8];

static uint32_t mockRead(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
	MockFunc& f = g_space[bus][dev][func];
	if (!f.present) return 0xffffffffu;
	return f.cfg[(off & 0xfc) / 4];
}
static void mockWrite(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val) {
	MockFunc& f = g_space[bus][dev][func];
	if (!f.present) return;
	int idx = (off & 0xfc) / 4;
	if (idx >= 4 && idx <= 9 && f.barMask[idx - 4]) {     // BAR0..5 sized region
		int b = idx - 4;
		f.cfg[idx] = (val & f.barMask[b]) | f.barFlags[b]; // only writable bits move; flags hardwired
	} else {
		f.cfg[idx] = val;
	}
}

static void resetSpace() {
	std::memset(g_space, 0, sizeof(g_space));
	Pci::setBackend(mockRead, mockWrite);
}

// Configure one device. clazz packs (class<<16)|(subclass<<8)|progIf into 0x08..0x0b alongside rev.
static MockFunc& addDev(uint8_t bus, uint8_t dev, uint8_t func, uint16_t vendor, uint16_t device,
                        uint8_t classCode, uint8_t subclass, uint8_t headerType, uint8_t irq) {
	MockFunc& f = g_space[bus][dev][func];
	f.present = true;
	f.cfg[0] = (uint32_t) device << 16 | vendor;                   // 0x00 device:vendor
	f.cfg[2] = (uint32_t) classCode << 24 | (uint32_t) subclass << 16; // 0x08 class/subclass/progif/rev
	f.cfg[3] = (uint32_t) headerType << 16;                        // 0x0c header type at byte 0x0e
	f.cfg[15] = irq;                                               // 0x3c IRQ line (byte 0)
	return f;
}

// Install a sized memory BAR. size must be a power of two >= 16.
static void setMemBar(MockFunc& f, int n, uint32_t base, uint32_t size, bool is64, bool prefetch) {
	uint32_t flags = (is64 ? 0x4u : 0x0u) | (prefetch ? 0x8u : 0x0u);
	f.barMask[n] = ~(size - 1);                  // writable address bits (clears low log2(size), incl flags)
	f.barFlags[n] = flags;
	f.cfg[4 + n] = (base & f.barMask[n]) | flags;
}
static void setIoBar(MockFunc& f, int n, uint32_t base, uint32_t size) {
	f.barMask[n] = ~(size - 1);
	f.barFlags[n] = 0x1u;                        // bit0 = I/O space
	f.cfg[4 + n] = (base & f.barMask[n]) | 0x1u;
}

// ----------------------------------------------------------------------------

TEST_CASE("empty bus: nothing enumerated, find fails") {
	resetSpace();
	PciDevice out[8];
	CHECK(Pci::enumerate(out, 8) == 0);
	PciDevice d;
	CHECK(Pci::find(0x8086, 0x100E, d) == false);
}

TEST_CASE("probe a single e1000: fields + BAR decode") {
	resetSpace();
	// e1000 82540EM: vendor 8086 device 100E, class 02 (network) subclass 00, header type 0, irq 11.
	MockFunc& f = addDev(0, 3, 0, 0x8086, 0x100E, 0x02, 0x00, 0x00, 11);
	setMemBar(f, 0, 0xfeb80000u, 128 * 1024, /*is64*/false, /*prefetch*/false);  // BAR0 regs, 128KiB
	setIoBar(f, 2, 0xc000u, 64);                                                  // BAR2 I/O, 64 bytes

	PciDevice d;
	REQUIRE(Pci::probe(0, 3, 0, d));
	CHECK(d.vendor == 0x8086);
	CHECK(d.device == 0x100E);
	CHECK(d.classCode == 0x02);
	CHECK(d.subclass == 0x00);
	CHECK(d.headerType == 0x00);
	CHECK(d.irqLine == 11);

	CHECK(d.bar[0].isIo == false);
	CHECK(d.bar[0].addr == 0xfeb80000u);
	CHECK(d.bar[0].size == 128u * 1024);
	CHECK(d.bar[0].is64 == false);

	CHECK(d.bar[2].isIo == true);
	CHECK(d.bar[2].addr == 0xc000u);
	CHECK(d.bar[2].size == 64u);
}

TEST_CASE("find by vendor:device locates the NIC anywhere on the bus") {
	resetSpace();
	addDev(0, 0, 0, 0x8086, 0x1237, 0x06, 0x00, 0x00, 0);   // host bridge (not it)
	MockFunc& f = addDev(0, 5, 0, 0x8086, 0x100E, 0x02, 0x00, 0x00, 11);
	setMemBar(f, 0, 0xfebc0000u, 128 * 1024, false, false);

	PciDevice d;
	REQUIRE(Pci::find(0x8086, 0x100E, d));
	CHECK(d.bus == 0);
	CHECK(d.dev == 5);
	CHECK(d.bar[0].addr == 0xfebc0000u);
}

TEST_CASE("enumerate counts every function, scanning multifunction devices") {
	resetSpace();
	addDev(0, 0, 0, 0x8086, 0x1237, 0x06, 0x00, /*header*/0x80, 0);  // multifunction header (bit7)
	addDev(0, 0, 1, 0x8086, 0x7000, 0x06, 0x01, 0x80, 0);            // func 1 present
	addDev(0, 2, 0, 0x1234, 0x1111, 0x03, 0x00, 0x00, 0);            // single-function VGA

	PciDevice out[16];
	int n = Pci::enumerate(out, 16);
	CHECK(n == 3);
}

TEST_CASE("non-multifunction device: funcs 1..7 are not probed") {
	resetSpace();
	addDev(0, 1, 0, 0x10ec, 0x8139, 0x02, 0x00, /*header*/0x00, 5);  // single function (bit7 clear)
	addDev(0, 1, 3, 0xdead, 0xbeef, 0x02, 0x00, 0x00, 5);            // present but must be ignored
	PciDevice out[8];
	int n = Pci::enumerate(out, 8);
	CHECK(n == 1);
	CHECK(out[0].device == 0x8139);
}

TEST_CASE("64-bit memory BAR: flagged and high slot skipped") {
	resetSpace();
	MockFunc& f = addDev(0, 4, 0, 0x1af4, 0x1000, 0x02, 0x00, 0x00, 10);
	setMemBar(f, 0, 0xfd000000u, 16 * 1024, /*is64*/true, /*prefetch*/true);
	// high dword of a 64-bit BAR lives in slot 1 (we leave it 0 = address fits in 32 bits)
	PciDevice d;
	REQUIRE(Pci::probe(0, 4, 0, d));
	CHECK(d.bar[0].is64 == true);
	CHECK(d.bar[0].prefetch == true);
	CHECK(d.bar[0].addr == 0xfd000000u);
	CHECK(d.bar[0].size == 16u * 1024);
	CHECK(d.bar[1].size == 0u);   // consumed by the 64-bit BAR, not decoded as its own region
}

TEST_CASE("enableBusMaster sets command bit 2 (and survives RMW of other bits)") {
	resetSpace();
	MockFunc& f = addDev(0, 3, 0, 0x8086, 0x100E, 0x02, 0x00, 0x00, 11);
	f.cfg[1] = 0x02900000u | 0x0000;   // status word high, command = 0 initially
	PciDevice d;
	REQUIRE(Pci::probe(0, 3, 0, d));
	Pci::enableBusMaster(d);
	Pci::enableMemSpace(d);
	uint16_t cmd = Pci::read16(0, 3, 0, PCI_COMMAND);
	CHECK((cmd & PCI_CMD_MASTER) != 0);
	CHECK((cmd & PCI_CMD_MEM) != 0);
	CHECK((Pci::read32(0, 3, 0, 0x04) >> 16) == 0x0290);   // status half preserved
}

TEST_CASE("8/16-bit config accessors derive correctly from the 32-bit backend") {
	resetSpace();
	addDev(0, 6, 0, 0x8086, 0x100E, 0x02, 0x00, 0x00, 9);
	CHECK(Pci::read16(0, 6, 0, PCI_VENDOR_ID) == 0x8086);
	CHECK(Pci::read16(0, 6, 0, PCI_DEVICE_ID) == 0x100E);
	CHECK(Pci::read8(0, 6, 0, PCI_CLASS) == 0x02);
	CHECK(Pci::read8(0, 6, 0, PCI_IRQ_LINE) == 9);
	// write8 to IRQ line, then read it back
	Pci::write8(0, 6, 0, PCI_IRQ_LINE, 0x2a);
	CHECK(Pci::read8(0, 6, 0, PCI_IRQ_LINE) == 0x2a);
	// write16 to a scratch register
	Pci::write16(0, 6, 0, 0x40, 0xbeef);
	CHECK(Pci::read16(0, 6, 0, 0x40) == 0xbeef);
}
