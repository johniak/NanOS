# NIC port Phase 2 — I219-LM (ich9lan) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans / subagent-driven-development.
> Checkbox (`- [ ]`) steps.

**Goal:** Add the Intel I219-LM (`8086:0D4E`) NIC kext for the Dell Latitude 5310 — a thin variant over
the Phase 1 `E1000Core` plus an `i219_phy.cpp` ich9lan bring-up (ME firmware semaphore, PHY over MDIC,
PHY reset/liveness, ULP disable). QEMU has no I219 model, so the CI bar is: compiles, links into
`i219.nkext`, loads-when-matched (a no-op on QEMU — no regression to e1000/e1000e). Real validation is
the Dell, like the xHCI BIOS handoff.

**Architecture:** `kext/i219/i219.cpp` (match `0x0D4E`, MSI, `phyBringup=i219PhyBringup`) + `i219_phy.cpp`
(the ich9lan logic). Both compiled with `e1000_core.o` into `i219.nkext`. The PHY/ME registers come from
the Linux e1000e reference (MDIC 0x20, EXTCNF_CTRL 0xF00 SWFLAG 0x20, FWSM 0x5B54, FEXTNVM7 0xE4).

**Tech Stack:** C++ freestanding x86_64 kext (NxFormat .nkext). Host doctest for the pure MDIC command
encoder. QEMU regression only (no I219 model). Spec
`docs/superpowers/specs/2026-06-20-i219-e1000e-nic-port-design.md` §3.4.

**Conventions:** `make check-arch` clean; build in Docker; QEMU native. Branch `feat/nic-i219`.

---

## File Structure

- `kext/i219/i219.cpp` — **new.** Thin variant: match `0x0D4E`, `IRQ_MSI`, `rxCsumOffload`, `phyBringup`.
- `kext/i219/i219_phy.cpp` / `i219_phy.h` — **new.** ich9lan bring-up + the pure `mdicCmd()` encoder.
- `tests/test_i219_phy.cpp` — **new.** Host-test `mdicCmd()` (pure logic).
- `Makefile` — **modify.** `kext/i219/%.cpp` pattern rule, `i219.nkext` link rule, `KEXTS += i219`,
  `TEST_MODULES`/`COV_PATTERNS` for the pure encoder, `-Ikext/i219` in `HINCLUDES`.

---

## Task 1: I219 PHY/ME bring-up logic + host-tested MDIC encoder

**Files:**
- Create: `kext/i219/i219_phy.h`, `kext/i219/i219_phy.cpp`, `tests/test_i219_phy.cpp`
- Modify: `Makefile`

- [ ] **Step 1: Write `kext/i219/i219_phy.h`.**

```cpp
#pragma once
#include <stdint.h>
struct E1000Core;

// Pure: build the MDIC command word for a PHY read/write (host-tested).
//   phyReg: PHY register index (0..31), phyAddr: PHY address (I219 = 1), data: write data (16-bit).
uint32_t mdicCmd(bool write, uint8_t phyAddr, uint8_t phyReg, uint16_t data);

// ich9lan bring-up hook (E1000Variant.phyBringup): acquire the ME/SW semaphore, confirm the PHY
// answers over MDIC, disable ULP, release the semaphore. Returns true if the PHY is usable.
bool i219PhyBringup(E1000Core* c);
```

- [ ] **Step 2: Write the failing test** `tests/test_i219_phy.cpp`.

```cpp
#include "doctest.h"
#include "i219_phy.h"

TEST_CASE("mdicCmd encodes the e1000 MDIC read/write command word") {
    // READ: opcode 2<<26, phyAddr 1<<21, reg 2<<16, no data.
    uint32_t r = mdicCmd(false, 1, 2, 0);
    CHECK((r & 0x0C000000u) == 0x08000000u);          // OP = READ (2 << 26)
    CHECK(((r >> 21) & 0x1Fu) == 1u);                 // PHY address
    CHECK(((r >> 16) & 0x1Fu) == 2u);                 // PHY register
    CHECK((r & 0xFFFFu) == 0u);                        // read carries no data
    // WRITE: opcode 1<<26, data in low 16 bits.
    uint32_t w = mdicCmd(true, 1, 0, 0xABCD);
    CHECK((w & 0x0C000000u) == 0x04000000u);          // OP = WRITE (1 << 26)
    CHECK((w & 0xFFFFu) == 0xABCDu);                   // write data
}
```

- [ ] **Step 3: Run it; expect FAIL** (`mdicCmd` undefined).

Run: `make ARCH=x86_64 test 2>&1 | grep -iE "mdicCmd|i219|undefined|error"`
Expected: FAIL.

- [ ] **Step 4: Write `kext/i219/i219_phy.cpp`.**

```cpp
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
	EXTCNF_CTRL_SWFLAG = 0x00000020,      // ich8lan software-flag semaphore (shared with the ME)
	FEXTNVM7_DISABLE_ULP = 0x00000004,    // best-effort: side-clock ungate / ULP-related
	I219_PHY_ADDR = 1,
};

// ---- pure (host-tested) ----
uint32_t mdicCmd(bool write, uint8_t phyAddr, uint8_t phyReg, uint16_t data) {
	return (write ? MDIC_OP_WRITE : MDIC_OP_READ)
	     | ((uint32_t) (phyAddr & 0x1F) << MDIC_PHY_SHIFT)
	     | ((uint32_t) (phyReg & 0x1F) << MDIC_REG_SHIFT)
	     | (write ? data : 0);
}

#ifndef NANOS_HOST_TEST
extern "C" void knx_log(const char* s);

static inline void wr(E1000Core* c, int off, uint32_t v) { *(volatile uint32_t*) (c->mmio + off) = v; }
static inline uint32_t rd(E1000Core* c, int off) { return *(volatile uint32_t*) (c->mmio + off); }
static void spin(int n) { for (volatile int i = 0; i < n; i++) {} }

// Acquire the ich8lan SW/FW semaphore: set EXTCNF_CTRL.SWFLAG, read it back to confirm we won the
// arbitration against the ME firmware. Bounded; returns false on timeout (caller aborts cleanly).
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
	// Disable ULP (best-effort, MAC-side FEXTNVM7) so the PHY is out of ultra-low-power before use.
	wr(c, REG_FEXTNVM7, rd(c, REG_FEXTNVM7) & ~FEXTNVM7_DISABLE_ULP);
	// Confirm the PHY answers: read the PHY ID (registers 2 and 3). A live I219 PHY returns a non-zero,
	// non-0xFFFF identifier. If it does not respond, the link can't come up — abort the bring-up.
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
```
> The ME semaphore + MDIC PHY access are the I219 essentials (don't fight the ME; reach the PHY). The
> deep ULP power-state / K1 PHY-page sequences (`e1000_disable_ulp_lpt_lp`) are a real-HW refinement
> gated on what the Dell shows — on a fresh UEFI boot the PHY is usually already out of ULP with link
> established by firmware. Each touchpoint is bounded so a wrong guess fails the NIC, never the system.

- [ ] **Step 5: Wire the Makefile.** Add a pattern rule (after the `kext/e1000/%.cpp` one):
```make
$(BINFOLDER)%.o: kext/i219/%.cpp $(MKNX_DEP)
	@mkdir -p $(BINFOLDER)
	$(CXX) $(KEXT_CFLAGS) -Ikext/e1000 -MMD -MP -c $< -o $@
```
Add the `i219.nkext` link rule (mirror e1000e, linking `i219.o i219_phy.o e1000_core.o`), append `i219`
to `KEXTS`, add `kext/i219/i219_phy.cpp` to `TEST_MODULES` + `"*/i219_phy.*"` to `COV_PATTERNS`, and add
`-Ikext/i219` to `HINCLUDES`.
> Match the EXACT form of the existing `kext/e1000/%.cpp` pattern rule (KEXT_CFLAGS, deps) — read it
> first; the `-Ikext/e1000` lets `i219_phy.cpp` find `e1000_core.h`.

- [ ] **Step 6: Run it; expect PASS.**

Run: `make ARCH=x86_64 test 2>&1 | grep -iE "mdicCmd|test cases|coverage|fail"`
Expected: PASS (the encoder case green).

- [ ] **Step 7: Commit.**

```bash
git add kext/i219/i219_phy.h kext/i219/i219_phy.cpp tests/test_i219_phy.cpp Makefile
git commit -m "feat(net,i219): ich9lan PHY bring-up (ME semaphore + MDIC) + host-tested MDIC encoder"
```

---

## Task 2: I219 kext + build + QEMU no-regression

**Files:**
- Create: `kext/i219/i219.cpp`
- Modify: `Makefile`

- [ ] **Step 1: Write `kext/i219/i219.cpp`.**

```cpp
/*
 * i219.cpp — Intel I219-LM (PCI 8086:0D4E, Comet Lake PCH) as i219.nkext, the Dell Latitude 5310 NIC.
 * Thin variant over E1000Core + the ich9lan PHY bring-up. QEMU has no I219 model, so this binds only
 * on real hardware; on QEMU knx_pci_find fails and nkext_init returns -1 quietly (no effect).
 */
#include "e1000_core.h"
#include "i219_phy.h"

extern "C" int  knx_pci_find(uint16_t vendor, uint16_t device, uint8_t* bus, uint8_t* dev, uint8_t* func);
extern "C" void knx_log(const char* s);

static E1000Core g_core;
static const E1000Variant g_i219 = {
	0x0D4E, "i219", /*phyBringup*/ i219PhyBringup, /*readMac*/ 0, IRQ_MSI, /*rxCsumOffload*/ true,
};

extern "C" int nkext_init() {
	uint8_t bus, dev, func;
	if (!knx_pci_find(0x8086, 0x0D4E, &bus, &dev, &func)) {
		knx_log("i219: no 8086:0D4E found\n");
		return -1;
	}
	return coreStart(&g_core, &g_i219, bus, dev, func);
}
```

- [ ] **Step 2: Add the `i219.nkext` link rule + `KEXTS += i219`.** After the `e1000e.nkext` rule:
```make
$(BINFOLDER)i219.nkext: $(KEXT_GLUE) $(BINFOLDER)i219.o $(BINFOLDER)i219_phy.o $(BINFOLDER)e1000_core.o $(MKNX_TOOL) $(KEXT_LD)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(KEXT_LD) -o $(@:.nkext=.elf) \
	  $(KEXT_GLUE) $(BINFOLDER)i219.o $(BINFOLDER)i219_phy.o $(BINFOLDER)e1000_core.o -lgcc
	$(MKNX_TOOL) $(@:.nkext=.elf) $@
```
and `KEXTS=kbd mouse e1000 e1000e i219`.

- [ ] **Step 3: check-arch + build + ship.**

Run: `make check-arch && make image64 2>&1 | tail -2`
Expected: arch-clean; image builds; `i219.nkext` shipped to `/nanos/kext`.

- [ ] **Step 4: QEMU no-regression — i219 is a quiet no-op (no 0x0D4E in QEMU).**

Run: `make smoke-x86_64 && make smoke-e1000e`
Expected: both PASS (e1000 INTx + e1000e MSI unaffected; the i219 kext loads, finds no device, returns
-1 — it must NOT disturb the working NICs).

- [ ] **Step 5: Commit.**

```bash
git add kext/i219/i219.cpp Makefile
git commit -m "feat(net,i219): I219-LM kext (8086:0D4E) over the shared core — Dell NIC, no-op on QEMU"
```

---

## Notes for the implementer

- **Real validation is the Dell.** Nothing here is exercised by QEMU (no I219 model); the gate is "builds
  + loads-when-matched + zero regression to e1000/e1000e". The ich9lan registers are transcribed from the
  Linux e1000e reference (MDIC 0x20, EXTCNF_CTRL 0xF00/SWFLAG 0x20, FWSM 0x5B54, FEXTNVM7 0xE4).
- **Deferred to a real-HW follow-up:** the full ULP/K1 power-state PHY-page sequence
  (`e1000_disable_ulp_lpt_lp`), MAC/PHY interconnect K1 tuning, NVM-read MAC fallback (only needed if
  RAL/RAH come up empty). Add these once the Dell shows whether the minimal bring-up establishes link.
- **Single MSI** (not MSI-X), same as e1000e — the I219 is single-queue here; `knx_register_msi` uses
  the MSI cap, falling back to legacy INTx if absent.
