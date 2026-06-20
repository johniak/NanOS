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
	(void) msixOff;
	// We use single-vector MSI (cap 0x05). The NIC is single-queue (one softirq), so all interrupt
	// causes funnel to one vector — MSI delivers that automatically with no per-chip routing. MSI-X
	// would add nothing here and needs device-specific IVAR cause->vector routing (a future
	// multi-queue/RSS refinement). If a device offers no MSI cap, the caller falls back to legacy INTx.
	if (!msiOff)
		return none;

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

}  // namespace kernel
