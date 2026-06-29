---
name: nanos-nic-e1000e-i219
description: "Intel I219-LM / e1000e NIC port — Phase 1 (shared core + LAPIC + MSI) + Phase 2 (I219 ich9lan kext) both DONE; I219 path validates only on the Dell"
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

NIC port toward networking on the Dell Latitude 5310 (Intel **I219-LM**, PCI `8086:0D4E`, Comet Lake-U
PCH-integrated — QEMU does NOT emulate it). Branch `feat/nic-i219`. Spec
`docs/superpowers/specs/2026-06-20-i219-e1000e-nic-port-design.md`, plan
`docs/superpowers/plans/2026-06-20-nic-phase1-core-msi-e1000e.md`.

**Phase 1 DONE + `make verify64` green (646 host tests + all 5 boot smokes incl. new `smoke-e1000e`).**
A shared **`E1000Core`** engine (`kext/e1000/e1000_core.{h,cpp}`: registers, RX/TX rings, NAPI drain,
RX-csum, variant hooks/flags) with three thin kexts over it. e1000 (`8086:100E`, INTx — the QEMU NIC)
refactored onto the core with behaviour byte-for-byte preserved (`smoke-x86_64` is the gate). e1000e
(`8086:10D3`, 82574L) is a NEW kext + the QEMU test vehicle (`qemu -device e1000e`): a real ping
round-trips over it (`2 packets received`), proving the whole core + MSI + LAPIC + NAPI path.

New infra: **minimal Local APIC** (`arch/x86_64/cpu/lapic_x86_64.cpp`, enable + spurious vector + MSI
vector pool 0x70–0x77) running ALONGSIDE the 8259 PIC; MSI vector IDT stubs `irq_msi0..7` (irq64.S) +
LAPIC-EOI branch in `irq_handler`; kernel export **`knx_register_msi`** (`kernel/MsiRouter.cpp`,
host-tested: PCI cap walk + program the cap + install a trampoline on the allocated vector).

**Key decisions (don't re-litigate):**
- **Single-vector MSI, NOT MSI-X.** The net core is single-queue (one softirq), so all causes funnel to
  one vector — plain MSI delivers that automatically with NO per-chip IVAR. MSI-X without IVAR routing
  binds but delivers ZERO interrupts (verified: ping got 0 replies until switched to MSI). MSI-X +
  IVAR is a future multi-queue/RSS refinement only. `msiSetup` uses MSI (cap 0x05); no-MSI device →
  fall back to legacy INTx.
- **LAPIC is enabled from `mmuInitKernel`'s tail, NOT `cpuInit`.** `Kernel::start` calls `cpuInit`
  (283) BEFORE `initPaging`/`mmuInitKernel` (222 is a separate `initPaging`), so at cpuInit the loader's
  temp 1 GiB identity map is live and the LAPIC MMIO (0xFEE00000, above RAM) is unmapped. Fix:
  `mmuInitKernel` maps the 0xFEE00000 page on the live kernel space then calls `lapicInit()`. Mapping it
  from lapicInit-at-cpuInit also corrupts (a late page-table alloc hands out a frame overlapping the
  live kernel stack → #GP at a BIOS-ROM garbage RIP 0xf000ff53).
- **RX checksum offload IN; TX offload + TSO DEFERRED** — TX csum/TSO need L3/L4 offsets the
  `knx_tx_fn` raw-bytes ABI doesn't carry (and TSO needs GSO the TCP stack lacks). A real ABI
  dependency, not a shortcut. TX checksums stay software (correct). NVM-write/WoL/full-IOAPIC out
  (risk / no ACPI S-states / MSI bypasses IOAPIC).

**Host-test wiring:** `HOST_CXXFLAGS` defines `-DNANOS_HOST_TEST`; engine halves of lapic/e1000_core
are `#ifndef NANOS_HOST_TEST`, pure helpers (msiVecAlloc, msiSetup, ringNext/rxDescDone/txEncode)
always compiled + gated in `COV_PATTERNS`.

**Phase 2 DONE (plan `docs/superpowers/plans/2026-06-20-nic-phase2-i219-ich9lan.md`, real-HW only).**
`kext/i219/i219.cpp` (match `0x0D4E`, MSI, rxCsum, phyBringup) + `kext/i219/i219_phy.cpp`: the ich9lan
bring-up — acquire the ME/SW semaphore (EXTCNF_CTRL 0xF00 bit SWFLAG 0x20, bounded), ungate the side
clock (FEXTNVM7 0xE4), confirm the PHY answers over MDIC (0x20; READY 1<<28, READ 2<<26, WRITE 1<<26,
reg<<16, phy<<21, PHY addr 1), release. Pure `mdicCmd()` is host-tested. Registers transcribed from the
Linux e1000e reference. Builds + links into `i219.nkext` + loads-when-matched; on QEMU `knx_pci_find`
fails → quiet no-op (smoke-x86_64 + smoke-e1000e stay green). **NOT yet run on the Dell** — that is the
only real validation, like the xHCI handoff. **Deferred real-HW refinements** (add if the Dell shows
link doesn't come up): the full `e1000_disable_ulp_lpt_lp` PHY-page ULP/K1 sequence, MAC/PHY K1 tuning,
NVM-read MAC fallback (only if RAL/RAH are empty). See
[[nanos-dual-boot-limine]] (Dell boot), [[nanos-real-hardware-roadmap]], [[nanos-x64-paging-carryforward]].
