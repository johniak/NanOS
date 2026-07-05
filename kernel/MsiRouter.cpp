#include "MsiRouter.h"

namespace kernel {

// Read a 16-bit config field by reading the containing dword and shifting (off may be 2-aligned).
static uint16_t cfg16(const MsiEnv& e, uint8_t b, uint8_t d, uint8_t f, uint8_t off) {
	return (uint16_t) (e.cfgRead(b, d, f, (uint8_t) (off & 0xFC)) >> ((off & 2) * 8));
}

// Write the high 16 bits of the dword at `dwordOff` (message-control lives at cap+2), preserving low.
static void writeMsgCtrl(const MsiEnv& e, uint8_t b, uint8_t d, uint8_t f, uint8_t dwordOff, uint16_t mc) {
	uint32_t cur = e.cfgRead(b, d, f, dwordOff);
	e.cfgWrite(b, d, f, dwordOff, (cur & 0x0000FFFFu) | ((uint32_t) mc << 16));
}

MsiResult msiSetup(const MsiEnv& env, uint8_t bus, uint8_t dev, uint8_t func) {
	MsiResult none = { MSI_NONE, -1, 0 };
	uint16_t status = cfg16(env, bus, dev, func, 0x06);
	if (!(status & 0x10))                                  // Status bit4: capability list present
		return none;
	uint8_t msiOff = 0, msixOff = 0;
	uint8_t off = (uint8_t) (env.cfgRead(bus, dev, func, 0x34) & 0xFC);   // capabilities pointer
	for (int guard = 0; off && guard < 48; guard++) {
		uint32_t hdr = env.cfgRead(bus, dev, func, off);
		uint8_t id = (uint8_t) (hdr & 0xFF);
		if (id == 0x11) msixOff = off;
		else if (id == 0x05) msiOff = off;
		off = (uint8_t) ((hdr >> 8) & 0xFC);               // next-capability pointer
	}
	// Prefer single-vector MSI (cap 0x05) when the device offers it — the NIC and the Dell's i915
	// both expose classic MSI, and a single vector is all our single-queue/one-handler model needs.
	// Only when a device offers MSI-X *but not* MSI (QEMU's virtio-pci devices) do we program MSI-X.
	if (msiOff) {
		int vec = env.allocVector();
		if (vec < 0)
			return none;
		uint32_t addr = 0xFEE00000u | ((uint32_t) env.lapicId() << 12);

		// MSI: message control at cap+2; addr at cap+4; data at cap+8 (64-bit cap) or cap+8 low (32-bit).
		uint16_t mc = cfg16(env, bus, dev, func, (uint8_t) (msiOff + 2));
		bool is64 = (mc & 0x80) != 0;             // message-control bit7: 64-bit address capable
		env.cfgWrite(bus, dev, func, (uint8_t) (msiOff + 4), addr);
		if (is64) {
			env.cfgWrite(bus, dev, func, (uint8_t) (msiOff + 8), 0);
			env.cfgWrite(bus, dev, func, (uint8_t) (msiOff + 0xC), (uint32_t) vec);
		} else {
			env.cfgWrite(bus, dev, func, (uint8_t) (msiOff + 8), (uint32_t) vec);
		}
		mc |= 0x1;                                // MSI Enable (message-control bit0)
		writeMsgCtrl(env, bus, dev, func, msiOff, mc);
		return MsiResult{ MSI_KIND_MSI, vec, msiOff };
	}

	// Single-vector MSI-X. The capability's Table BIR+Offset (cap+4) locate the MSI-X table inside
	// one of the function's memory BARs; we map it, program table entry 0 to raise `vec` at the
	// LAPIC, unmask that entry, and enable the capability (clearing the global function mask). The
	// *device* still needs told which entry each queue uses (e.g. vp_modern_queue_vector for virtio)
	// — that is the driver/transport's job; msiSetup only wires the PCI side and the CPU vector.
	if (msixOff) {
		uint16_t mc      = cfg16(env, bus, dev, func, (uint8_t) (msixOff + 2));
		uint32_t tbir    = env.cfgRead(bus, dev, func, (uint8_t) (msixOff + 4));
		uint8_t  bir     = (uint8_t) (tbir & 0x7);                 // bits2:0 = BAR index
		uint32_t tblOff  = tbir & ~0x7u;                           // bits31:3 = 8-byte-aligned offset
		uint32_t barLo   = env.cfgRead(bus, dev, func, (uint8_t) (0x10 + bir * 4));
		if (barLo & 0x1)                                           // I/O BAR: MSI-X table must be MMIO
			return none;
		uint64_t barBase = (uint64_t) (barLo & ~0xFu);            // mask memory-BAR flag bits (low 4)
		if (((barLo >> 1) & 0x3) == 0x2) {                        // 64-bit BAR: fold in the high dword
			uint32_t barHi = env.cfgRead(bus, dev, func, (uint8_t) (0x10 + (bir + 1) * 4));
			barBase |= (uint64_t) barHi << 32;                   // e.g. i915's MSI-X table in BAR0
		}
		int vec = env.allocVector();
		if (vec < 0)
			return none;
		volatile uint32_t* e = (volatile uint32_t*) env.mapMmio(barBase + tblOff, 16);  // entry 0 = 16 B
		if (!e)
			return none;
		uint32_t addr = 0xFEE00000u | ((uint32_t) env.lapicId() << 12);
		e[0] = addr;                 // Message Address low
		e[1] = 0;                    // Message Address high
		e[2] = (uint32_t) vec;       // Message Data = the CPU vector
		e[3] = 0;                    // Vector Control: bit0 = mask; 0 = unmasked
		mc |=  0x8000;               // MSI-X Enable (message-control bit15)
		mc &= ~0x4000;               // clear Function Mask (bit14)
		writeMsgCtrl(env, bus, dev, func, msixOff, mc);
		return MsiResult{ MSI_KIND_MSIX, vec, msixOff };
	}

	// Neither MSI nor MSI-X: the caller falls back to legacy INTx via knx_register_irq.
	return none;
}

}  // namespace kernel
