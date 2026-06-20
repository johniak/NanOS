#include "i219_phy.h"
#include "e1000_core.h"

// ich8lan/ich9lan MAC registers (Linux e1000e regs.h) + MDIC bit fields (defines.h).
enum {
	REG_CTRL_EXT = 0x00018, REG_MDIC = 0x00020, REG_FEXTNVM7 = 0x000E4,
	REG_EXTCNF_CTRL = 0x00F00, REG_SWSM = 0x05B50, REG_FWSM = 0x05B54,
};
enum {
	MDIC_REG_SHIFT = 16, MDIC_PHY_SHIFT = 21,
	MDIC_OP_WRITE = 0x04000000, MDIC_OP_READ = 0x08000000,
	MDIC_READY = 0x10000000, MDIC_ERROR = 0x40000000,
	EXTCNF_CTRL_SWFLAG = 0x00000020,      // ich8lan software-flag semaphore (arbitrated with the ME)
	FEXTNVM7_SIDE_CLK_UNGATE = 0x00000004,
	I219_PHY_ADDR = 1,
};

// ---- pure (host-tested) ----
uint32_t mdicCmd(bool write, uint8_t phyAddr, uint8_t phyReg, uint16_t data) {
	return (write ? MDIC_OP_WRITE : MDIC_OP_READ)
	     | ((uint32_t) (phyAddr & 0x1F) << MDIC_PHY_SHIFT)
	     | ((uint32_t) (phyReg & 0x1F) << MDIC_REG_SHIFT)
	     | (write ? (uint32_t) data : 0u);
}

#ifndef NANOS_HOST_TEST
extern "C" void knx_log(const char* s);

static inline void wr(E1000Core* c, int off, uint32_t v) { *(volatile uint32_t*) (c->mmio + off) = v; }
static inline uint32_t rd(E1000Core* c, int off) { return *(volatile uint32_t*) (c->mmio + off); }
static void spin(int n) { for (volatile int i = 0; i < n; i++) {} }

// Acquire the ich8lan SW/FW semaphore: set EXTCNF_CTRL.SWFLAG and read it back to confirm we won the
// arbitration against the ME firmware. Bounded; false on timeout (caller aborts cleanly).
static bool acquireSwFlag(E1000Core* c) {
	for (int t = 0; t < 200; t++) {
		uint32_t e = rd(c, REG_EXTCNF_CTRL);
		wr(c, REG_EXTCNF_CTRL, e | EXTCNF_CTRL_SWFLAG);
		if (rd(c, REG_EXTCNF_CTRL) & EXTCNF_CTRL_SWFLAG)
			return true;
		spin(20000);
	}
	return false;
}
static void releaseSwFlag(E1000Core* c) {
	wr(c, REG_EXTCNF_CTRL, rd(c, REG_EXTCNF_CTRL) & ~EXTCNF_CTRL_SWFLAG);
}

// Read a PHY register over MDIC (bounded poll for READY; -1 on error/timeout).
static int phyRead(E1000Core* c, uint8_t reg) {
	wr(c, REG_MDIC, mdicCmd(false, I219_PHY_ADDR, reg, 0));
	for (int t = 0; t < 100000; t++) {
		uint32_t m = rd(c, REG_MDIC);
		if (m & MDIC_READY)
			return (m & MDIC_ERROR) ? -1 : (int) (m & 0xFFFF);
	}
	return -1;
}

bool i219PhyBringup(E1000Core* c) {
	if (!acquireSwFlag(c)) {
		knx_log("i219: could not acquire ME/SW semaphore\n");
		return false;
	}
	// Best-effort: ungate the side clock so the PHY is reachable out of ultra-low-power. The full
	// e1000_disable_ulp_lpt_lp PHY-page sequence is a real-HW refinement (a fresh UEFI boot usually
	// leaves the PHY out of ULP with link already established by firmware).
	wr(c, REG_FEXTNVM7, rd(c, REG_FEXTNVM7) | FEXTNVM7_SIDE_CLK_UNGATE);
	// Confirm the PHY answers: a live I219 PHY returns a non-zero, non-0xFFFF identifier at reg 2.
	int id2 = phyRead(c, 2);
	bool alive = (id2 > 0 && id2 != 0xFFFF);
	releaseSwFlag(c);
	if (!alive) {
		knx_log("i219: PHY not responding over MDIC\n");
		return false;
	}
	knx_log("i219: PHY up (ich9lan semaphore + MDIC)\n");
	return true;
}
#endif  // NANOS_HOST_TEST
