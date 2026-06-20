# Intel I219-LM / e1000e NIC port — design

**Date:** 2026-06-20
**Branch:** `feat/nic-i219` (off `feat/real-hw-boot`; x86_64 sole dev arch).
**Status:** design — pending user review of this spec.

---

## 1. Goal & scope

Give NanOS a working Gigabit NIC on the **Dell Latitude 5310** (Intel **I219-LM**, PCI `8086:0D4E`,
Comet Lake-U PCH-integrated MAC+PHY) — a *faithful*, Linux-`e1000e`-style driver: **MSI-X** (with MSI
then legacy INTx fallback, exactly Linux's try-order) and **NAPI-style** budgeted polling, the
**ich9lan** I219 bring-up (ME firmware semaphore, PHY over MDIC, ULP disable, K1/Comet Lake quirks), and
**RX/TX checksum offload**.

The existing **e1000** kext (82540EM, `8086:100E`, the QEMU NIC) **stays working and untouched in
behaviour** — it is *refactored* onto a shared core but its QEMU smoke must stay green. A new
**e1000e** kext (82574L, `8086:10D3`) is added **as the QEMU test vehicle**: QEMU emulates
`-device e1000e`, so the shared core + MSI-X + NAPI + checksum-offload path gets real headless coverage;
only the I219-specific ME/PHY/ULP quirks are real-hardware-only (like the xHCI BIOS handoff).

The driver work pulls one new foundational dependency into scope, by deliberate choice ("like Linux"):
a **minimal LAPIC** (enable + spurious vector) plus **MSI/MSI-X vector allocation**, living in
`arch/x86_64`, running **alongside the existing 8259 PIC** (PIC keeps the timer/keyboard/ATA legacy
IRQs; the LAPIC only receives the NIC's MSIs). No IOAPIC.

### Out of scope — with reasons (not laziness)

- **TSO (TCP segmentation offload):** blocked by a dependency — the NanOS TCP stack segments in software
  and hands the driver ≤MTU frames, so there is no super-MTU segment for the NIC to split. TSO would be
  dead code until the stack grows GSO. A separate later step.
- **Full IOAPIC:** MSI-X bypasses the IOAPIC (the message goes straight to the LAPIC); the NIC does not
  need it. It is a separate foundational subsystem (APIC/SMP roadmap), not part of a NIC port.
- **NVM *write*:** off the datapath and able to permanently corrupt the card's MAC/config. NVM *read*
  **is** in scope, but only as a MAC fallback.
- **WoL (Wake-on-LAN):** requires ACPI sleep states (S3/S4) NanOS does not have — nothing to wake from,
  so it is dead code.
- **MSI-X multi-queue / RSS:** one RX + one TX queue (single data vector + one "other/link" vector),
  matching our single-threaded softirq net core.

---

## 2. Architecture

```
                       net core (MI: NetDevice/NetProc/Arp/Ip/Tcp)  — UNCHANGED
                                   ▲ knx_add_net_dev / knx_netif_rx / tx callback
                                   │  (stable C ABI in kernel/knx_net.h)
   ┌───────────────────────────────┴───────────────────────────────┐
   │                   E1000Core (kext/e1000/e1000_core.{h,cpp})     │  shared, compiled into each kext
   │   registers, RX/TX rings, MAC init, NAPI poll, checksum-offload │
   │   variant hooks: ops{ matchPci, phyBringup, readMac, irqSetup } │
   └───────┬───────────────────┬───────────────────────┬────────────┘
           │ thin              │ thin                   │ thin
   ┌───────▼──────┐   ┌────────▼────────┐      ┌────────▼─────────────────┐
   │ e1000.cpp    │   │ e1000e.cpp      │      │ i219.cpp + i219_phy.cpp  │
   │ 8086:100E    │   │ 8086:10D3 82574L│      │ 8086:0D4E I219-LM        │
   │ legacy INTx  │   │ MSI-X (QEMU!)   │      │ MSI-X + ich9lan quirks   │
   │ QEMU/old HW  │   │ QEMU test veh.  │      │ the Dell (real-HW only)  │
   └──────────────┘   └─────────────────┘      └──────────────────────────┘

   arch/x86_64: lapic_x86_64.cpp (minimal LAPIC) + MSI vector alloc  ← new
   kernel: knx_register_msi() export (finds MSI/MSI-X cap, allocs vector, programs cap)  ← new
```

Each kext probes its own PCI id; only the one whose device is present initializes (the others
`nkext_init` returns `-1` quietly, exactly as e1000 does on a box without `100E`). All three ship to
`/nanos/kext`; `loadAllKexts` loads them all and each self-selects. No conflict: in QEMU the e1000 (or
e1000e, if launched with `-device e1000e`) wins; on the Dell the i219 wins.

---

## 3. Components

### 3.1 `E1000Core` — `kext/e1000/e1000_core.{h,cpp}` (the shared engine)

Holds everything the three variants share, lifted verbatim where possible from today's `e1000.cpp`
(register map, `RxDesc`/`TxDesc`, RX/TX rings, `rxPoll`, `e1000Tx`, `setupRings`, `readMac` default).
State moves from file-statics into a `struct E1000Core` instance (each kext owns one), so the three
drivers do not share mutable globals.

```cpp
struct E1000Variant {                 // a thin kext fills this and calls coreStart()
    uint16_t pciDevice;               // 0x100E / 0x10D3 / 0x0D4E
    const char* tag;                  // "e1000" / "e1000e" / "i219" (logging)
    // Optional hooks (null = use the core default):
    bool (*phyBringup)(E1000Core*);   // i219: ME semaphore + PHY/MDIC + ULP/K1; null elsewhere
    bool (*readMac)(E1000Core*);      // null = read RAL0/RAH0 (the e1000 default)
    int  irqMode;                     // IRQ_MSIX | IRQ_MSI | IRQ_INTX (preference order tried down)
    bool csumOffload;                 // enable RX/TX checksum offload (82574L + i219: true)
};

struct E1000Core {
    volatile uint8_t* mmio;
    /* rings, buffers, cursors, mac, handle — as in today's e1000.cpp, now per-instance */
    uint8_t bus, dev, func;
    const E1000Variant* v;
};

// One entry point a thin kext calls from nkext_init after matchPci succeeds:
int  coreStart(E1000Core* c, const E1000Variant* v);   // reset, rings, MAC, phyBringup, irqSetup, publish
void coreRxPoll(E1000Core* c);                          // NAPI drain (called from the ISR/MSI handler)
int  coreTx(E1000Core* c, const void* data, int len);   // the knx_tx_fn body
```

**Interrupt setup (`irqSetup`, in the core)** tries, in order: MSI-X → MSI → legacy INTx, stopping at
the first that succeeds, then unmasks the device causes (`IMS`). MSI/MSI-X go through the new
`knx_register_msi`; INTx through the existing `knx_register_irq(knx_pci_irq(...))`. The ISR/MSI handler
body is the same `coreRxPoll` NAPI drain regardless of delivery mode.

**Checksum offload (`csumOffload`)**: when set, program `RXCSUM` (IP/TCP/UDP checksum check) and use the
TX checksum-offload descriptor fields (`TXD` `IXSM/TXSM` via a context descriptor). The core marks
verified-good RX frames and lets the stack skip its software check; on TX it sets the offload bits
instead of having the stack pre-compute. Disabled (`false`) for the plain e1000 (82540EM) which the
stack already drives in software — **so the e1000 behaviour is byte-for-byte unchanged**.

### 3.2 `e1000.cpp` (refactored, behaviour-preserving)

Becomes a ~30-line thin wrapper: a static `E1000Variant{ .pciDevice=0x100E, .tag="e1000",
.irqMode=IRQ_INTX, .csumOffload=false }`, `nkext_init` → `knx_pci_find(0x8086,0x100E,…)` → `coreStart`.
No PHY hook (RAL/RAH MAC, link via `CTRL_SLU`). The QEMU smoke (`smoke-x86_64`) is the regression gate.

### 3.3 `e1000e.cpp` (new — the QEMU test vehicle)

Thin wrapper for the **82574L** (`8086:10D3`): `.irqMode=IRQ_MSIX`, `.csumOffload=true`, default RAL/RAH
MAC, no ich9lan PHY dance (the 82574L is a discrete PCIe NIC, not PCH-integrated). Lets QEMU
`-device e1000e` exercise the **entire** shared core + MSI-X + NAPI + checksum path headless.

### 3.4 `i219.cpp` + `i219_phy.cpp` (new — the Dell target)

`i219.cpp`: thin wrapper for `8086:0D4E`, `.irqMode=IRQ_MSIX`, `.csumOffload=true`,
`.phyBringup=i219PhyBringup`. `i219_phy.cpp` holds the **ich9lan** real-HW logic:

- **ME firmware semaphore (`SW_FW_SYNC` via `EXTCNF_CTRL`/`SWSM`)** acquired around *every* PHY/NVM
  access, released after — I219 shares the PHY and NVM with the Management Engine; skipping this races
  the ME and wedges the PHY.
- **PHY over MDIC** (register `0x20`): read/write PHY registers through the MDI control register with a
  ready-bit poll + timeout.
- **ULP (Ultra Low Power) disable** + the **K1 / Comet Lake** power-state workarounds and ring-flush
  sequence before bring-up (the ich8lan/ich9lan errata path).
- **MAC**: prefer `RAL0/RAH0` (BIOS-populated); fall back to a **NVM *read*** of the MAC words if they
  are zero/invalid.

If the PHY does not come up (link never establishes), `phyBringup` logs and returns `false` →
`coreStart` aborts init cleanly (the system keeps running, NIC just absent). This is the only path that
genuinely cannot be exercised in QEMU; its real test is the Dell.

### 3.5 `arch/x86_64` minimal LAPIC + MSI vectors

- `arch/x86_64/cpu/lapic_x86_64.cpp` (+ a contract addition under `arch/include/arch/`): enable the
  LAPIC (`IA32_APIC_BASE` MSR + spurious-interrupt-vector register), with an allocator handing out a
  small pool of MSI vectors (e.g. 0x70–0x7F) above the PIC range. Coexists with the 8259 PIC.
- IDT: route the allocated MSI vectors to a trampoline that calls the registered per-vector handler
  (the core's `coreRxPoll` NAPI body), then LAPIC EOI.

### 3.6 Kernel export — `knx_register_msi`

New `KX(knx_register_msi)` in `kexports.def` + `KernelExports.cpp`:

```c
// Allocate an interrupt for a PCI function via MSI-X (preferred) or MSI, install `h(ctx)` as the
// handler, program the device's MSI/MSI-X capability (LAPIC address + allocated vector) and enable it.
// Returns 0 on success, <0 if neither MSI-X nor MSI is available (caller falls back to legacy INTx).
int knx_register_msi(uint8_t bus, uint8_t dev, uint8_t func, void (*h)(void*), void* ctx);
```

The kernel side walks the PCI capability list (via the existing `knx_pci_cfg_read32/write32`), finds the
MSI-X (id 0x11) or MSI (id 0x05) cap, allocates a LAPIC vector, installs the handler, and programs the
cap. For MSI-X it maps the table BAR and writes the single table entry. This keeps all LAPIC/vector
knowledge in the kernel; the kext stays portable (asks for "an interrupt", gets a callback).

### 3.7 Build / packaging

`Makefile`: add `kext/e1000/%.cpp` and `kext/i219/%.cpp` pattern rules (e1000 dir rule already exists),
add link rules for `e1000e.nkext` (`e1000e.o + e1000_core.o`), `i219.nkext`
(`i219.o + i219_phy.o + e1000_core.o`), refactor `e1000.nkext` to also link `e1000_core.o`, and append
`e1000e i219` to `KEXTS`. All three land in `/nanos/kext` via the existing `_kext` + image steps.

---

## 4. Data flow

1. Boot → `loadAllKexts` loads `e1000.nkext`, `e1000e.nkext`, `i219.nkext`.
2. Each `nkext_init` → `knx_pci_find(0x8086, <its id>)`. On QEMU only e1000 (or e1000e with
   `-device e1000e`) matches; on the Dell only i219 matches. Non-matches return `-1` quietly.
3. Match → `coreStart`: bus-master enable, map BAR0, reset, `readMac`, `phyBringup` (i219 only),
   `setupRings` (DMA), program RCTL/TCTL/RXCSUM, `irqSetup` (MSI-X→MSI→INTx), publish `eth0` via
   `knx_add_net_dev`.
4. **RX:** NIC DMAs a frame → MSI-X message → LAPIC → trampoline → `coreRxPoll` drains the ring
   (`RXD_DD`), hands each frame to `knx_netif_rx` (enqueue to softirq thread; **no stack work in the
   interrupt**), recycles the descriptor (`RDT`). NAPI: the handler drains until the ring is empty
   within a budget, then returns.
5. **TX:** stack → `knx_tx_fn` → `coreTx` copies into the next TX buffer, sets the descriptor (+ offload
   bits when enabled), bumps `TDT`, bounded-waits for `TXD_DD`.

---

## 5. Error handling & fallbacks

- **No matching PCI device** → `nkext_init` returns `-1`, logged once; zero effect on the rest of the
  system (the established kext pattern).
- **MSI-X unavailable** → try MSI → try legacy INTx (`knx_register_irq`). If all fail, log and abort
  init (NIC absent, system fine). This mirrors Linux's degradation order.
- **ME semaphore timeout (i219)** → log + abort init; never spin forever (bounded poll), never wedge the
  box.
- **DMA alloc / BAR map failure** → abort init with a clear log (as today).
- **TX ring full / device wedged** → bounded `TXD_DD` wait then return error (as today), so a stuck NIC
  cannot hang the softirq thread.

---

## 6. Testing & verification

- **Host doctest** (`tests/test_e1000_core.cpp`): the parts of `E1000Core` that are pure logic over a
  fake MMIO/descriptor buffer — ring index advance, RX drain stopping at `!DD`, TX descriptor field
  encoding (incl. offload bits), the MSI-X-then-MSI-then-INTx selection given a fake capability list.
  No hardware. Added to `TEST_MODULES`/`COV_PATTERNS` (≥90%).
- **QEMU — full core + MSI-X + NAPI + checksum (the key coverage win):** new `make smoke-e1000e` boots
  with `-device e1000e` (82574L) instead of the default e1000, then asserts the net stack comes up over
  it — DHCP/ARP + a ping (reuse the existing net-smoke machinery). Proves the shared core, MSI-X
  delivery via the new LAPIC, NAPI drain, and checksum offload on real emulated silicon.
- **QEMU — e1000 regression:** existing `make smoke-x86_64` (default `-device e1000`) stays green after
  the refactor onto the core — the behaviour-preservation gate.
- **I219:** compiles, links into `i219.nkext`, and loads-when-matched; the ME/PHY/ULP path is
  **real-hardware-only** (the Dell). CI bar = QEMU green (e1000 + e1000e) + i219 builds/links; the Dell
  is the real validation, like the xHCI handoff.
- **`make check-arch`** clean (the LAPIC lives in `arch/x86_64`; the kexts are MD ring-0 outside the MI
  guard, as today). `make verify64` extended with `smoke-e1000e`.

---

## 7. File inventory

**New:** `kext/e1000/e1000_core.{h,cpp}`, `kext/e1000/e1000e.cpp`, `kext/i219/i219.cpp`,
`kext/i219/i219_phy.cpp`, `arch/x86_64/cpu/lapic_x86_64.cpp` (+ a contract header under
`arch/include/arch/`), `tests/test_e1000_core.cpp`, `scripts/smoke-e1000e.sh`.

**Modified:** `kext/e1000/e1000.cpp` (thin wrapper over the core — behaviour preserved),
`kernel/kexports.def` + `kernel/KernelExports.cpp` (`knx_register_msi`), `arch/x86_64` IDT wiring for MSI
vectors, `Makefile` (`KEXTS`, kext build/link rules, `smoke-e1000e`, `verify64`).

**Untouched in behaviour:** the MI net core (`net/*`), `knx_net.h` ABI (only a new export is added, no
change to existing ones).

---

## 8. Risks

- **Refactor regression on e1000** — mitigated by the unchanged `smoke-x86_64` gate and
  `csumOffload=false` keeping the 82540EM path identical.
- **LAPIC + 8259 coexistence** — enabling the LAPIC must not disturb the PIC-delivered legacy IRQs;
  verified by `smoke-e1000e` (which needs both the PIC timer/console *and* LAPIC MSI working at once).
- **I219 ich9lan correctness** — the single genuinely unverifiable-in-QEMU piece; written from the Linux
  `e1000e` ich8lan/ich9lan reference, validated only on the Dell. Bounded everywhere so a wrong guess
  fails the NIC, not the system.
