#include "Pci.h"

namespace kernel {

static PciCfgRead32  g_rd = 0;
static PciCfgWrite32 g_wr = 0;

void Pci::setBackend(PciCfgRead32 rd, PciCfgWrite32 wr) { g_rd = rd; g_wr = wr; }
bool Pci::hasBackend() { return g_rd && g_wr; }

uint32_t Pci::read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
	return g_rd ? g_rd(bus, dev, func, off & 0xfc) : 0xffffffffu;
}
void Pci::write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t v) {
	if (g_wr) g_wr(bus, dev, func, off & 0xfc, v);
}

uint16_t Pci::read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
	uint32_t d = read32(bus, dev, func, off);
	return (uint16_t) ((d >> ((off & 2) * 8)) & 0xffff);
}
uint8_t Pci::read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
	uint32_t d = read32(bus, dev, func, off);
	return (uint8_t) ((d >> ((off & 3) * 8)) & 0xff);
}
void Pci::write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint16_t v) {
	uint32_t d = read32(bus, dev, func, off);
	unsigned sh = (off & 2) * 8;
	d = (d & ~(0xffffu << sh)) | ((uint32_t) v << sh);
	write32(bus, dev, func, off, d);
}
void Pci::write8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint8_t v) {
	uint32_t d = read32(bus, dev, func, off);
	unsigned sh = (off & 3) * 8;
	d = (d & ~(0xffu << sh)) | ((uint32_t) v << sh);
	write32(bus, dev, func, off, d);
}

// Decode all six BARs: read flags, size via the canonical write-all-ones / read-back / restore.
void Pci::readBars(PciDevice& d) {
	for (int i = 0; i < 6; i++) { d.bar[i].addr = 0; d.bar[i].size = 0; d.bar[i].isIo = false;
	                              d.bar[i].is64 = false; d.bar[i].prefetch = false; }
	// Only type-0 headers (general devices) have 6 BARs; bridges (type 1) have 2. We read up to
	// the header's count; the e1000 we care about is type 0.
	int nbars = ((d.headerType & 0x7f) == 0) ? 6 : 2;
	for (int i = 0; i < nbars; i++) {
		uint8_t off = (uint8_t) (PCI_BAR0 + i * 4);
		uint32_t orig = read32(d.bus, d.dev, d.func, off);
		if (orig == 0)
			continue;                          // unimplemented BAR
		if (orig & 0x1) {
			// I/O-space BAR: bit0=1, base = val & ~0x3.
			d.bar[i].isIo = true;
			d.bar[i].addr = orig & ~0x3u;
			write32(d.bus, d.dev, d.func, off, 0xffffffffu);
			uint32_t back = read32(d.bus, d.dev, d.func, off) & ~0x3u;
			write32(d.bus, d.dev, d.func, off, orig);
			d.bar[i].size = back ? (~back + 1) & 0xffff : 0;   // I/O regions are <=64KiB
		} else {
			// Memory BAR: bits[2:1] type (0=32, 2=64), bit3 prefetchable. A 64-bit BAR spans this
			// slot (lo) plus the next (hi); we decode both into bar[lo] and skip hi.
			int lo = i;
			uint32_t type = (orig >> 1) & 0x3;
			d.bar[lo].prefetch = (orig >> 3) & 0x1;
			d.bar[lo].is64 = (type == 0x2);
			// Low dword: base (mask flag bits) + size probe.
			write32(d.bus, d.dev, d.func, off, 0xffffffffu);
			uint32_t backLo = read32(d.bus, d.dev, d.func, off) & ~0xfu;
			write32(d.bus, d.dev, d.func, off, orig);
			uint64_t addr = (uint64_t) (orig & ~0xfu);
			// Assemble the 64-bit size mask. For a 32-bit BAR the high bits don't exist, so treat
			// them as all-ones — otherwise (~mask + 1) would compute a bogus >4 GiB size.
			uint32_t backHi = 0;
			uint64_t mask = (uint64_t) backLo | 0xffffffff00000000ull;
			if (d.bar[lo].is64 && lo + 1 < 6) {
				// The high dword occupies the next slot: it holds the top 32 bits of both the base
				// and the size mask. Probe + restore it too, fold it in, then skip the slot.
				uint8_t offHi = (uint8_t) (off + 4);
				uint32_t origHi = read32(d.bus, d.dev, d.func, offHi);
				addr |= (uint64_t) origHi << 32;
				write32(d.bus, d.dev, d.func, offHi, 0xffffffffu);
				backHi = read32(d.bus, d.dev, d.func, offHi);
				write32(d.bus, d.dev, d.func, offHi, origHi);
				mask = (uint64_t) backLo | ((uint64_t) backHi << 32);
				i++;   // skip the high slot
			}
			d.bar[lo].addr = addr;
			// A read-back of 0 (both dwords for a 64-bit BAR) means an unimplemented/zero-size BAR;
			// only then is size 0 — otherwise the region is (~mask + 1). backLo can legitimately be 0
			// for a 64-bit BAR whose size is >= 4 GiB (all its size bits live in the high dword).
			bool sized = backLo != 0 || (d.bar[lo].is64 && backHi != 0);
			d.bar[lo].size = sized ? (~mask + 1) : 0;
		}
	}
}

bool Pci::probe(uint8_t bus, uint8_t dev, uint8_t func, PciDevice& out) {
	uint16_t vendor = read16(bus, dev, func, PCI_VENDOR_ID);
	if (vendor == PCI_VENDOR_NONE || vendor == 0x0000)
		return false;
	out.bus = bus; out.dev = dev; out.func = func;
	out.vendor = vendor;
	out.device     = read16(bus, dev, func, PCI_DEVICE_ID);
	out.revision   = read8(bus, dev, func, PCI_REVISION);
	out.progIf     = read8(bus, dev, func, PCI_PROG_IF);
	out.subclass   = read8(bus, dev, func, PCI_SUBCLASS);
	out.classCode  = read8(bus, dev, func, PCI_CLASS);
	out.headerType = read8(bus, dev, func, PCI_HEADER_TYPE);
	out.irqLine    = read8(bus, dev, func, PCI_IRQ_LINE);
	out.irqPin     = read8(bus, dev, func, PCI_IRQ_PIN);
	readBars(out);
	return true;
}

int Pci::enumerate(PciDevice* out, int max) {
	int n = 0;
	for (int bus = 0; bus < 256; bus++) {
		for (int dev = 0; dev < 32; dev++) {
			PciDevice fn0;
			if (!probe((uint8_t) bus, (uint8_t) dev, 0, fn0))
				continue;
			if (n < max) out[n] = fn0;
			n++;
			// Multifunction (header bit7) → scan functions 1..7.
			if (fn0.headerType & 0x80) {
				for (int func = 1; func < 8; func++) {
					PciDevice fn;
					if (probe((uint8_t) bus, (uint8_t) dev, (uint8_t) func, fn)) {
						if (n < max) out[n] = fn;
						n++;
					}
				}
			}
		}
	}
	return n;
}

bool Pci::find(uint16_t vendor, uint16_t device, PciDevice& out) {
	for (int bus = 0; bus < 256; bus++) {
		for (int dev = 0; dev < 32; dev++) {
			PciDevice fn0;
			if (!probe((uint8_t) bus, (uint8_t) dev, 0, fn0))
				continue;
			int funcs = (fn0.headerType & 0x80) ? 8 : 1;
			for (int func = 0; func < funcs; func++) {
				PciDevice d;
				if (!probe((uint8_t) bus, (uint8_t) dev, (uint8_t) func, d))
					continue;
				if ((vendor == 0xFFFF || d.vendor == vendor) &&
				    (device == 0xFFFF || d.device == device)) {
					out = d;
					return true;
				}
			}
		}
	}
	return false;
}

void Pci::enableBusMaster(const PciDevice& d) {
	uint16_t cmd = read16(d.bus, d.dev, d.func, PCI_COMMAND);
	cmd |= PCI_CMD_MASTER;
	write16(d.bus, d.dev, d.func, PCI_COMMAND, cmd);
}
void Pci::enableMemSpace(const PciDevice& d) {
	uint16_t cmd = read16(d.bus, d.dev, d.func, PCI_COMMAND);
	cmd |= PCI_CMD_MEM;
	write16(d.bus, d.dev, d.func, PCI_COMMAND, cmd);
}
void Pci::enableIoSpace(const PciDevice& d) {
	uint16_t cmd = read16(d.bus, d.dev, d.func, PCI_COMMAND);
	cmd |= PCI_CMD_IO;
	write16(d.bus, d.dev, d.func, PCI_COMMAND, cmd);
}

}  // namespace kernel
