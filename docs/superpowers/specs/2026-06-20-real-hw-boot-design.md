# Real-hardware boot bring-up — 64-bit memory map + xHCI firmware handoff — design

**Date:** 2026-06-20
**Branch:** `feat/real-hw-boot` (off `feat/boot-uefi-bios`; x86_64 sole dev arch, i686 frozen).
**Status:** design approved, ready for the implementation plan.

---

## 1. Goal & scope

Close the two **real-silicon-vs-QEMU gaps** that block booting NanOS on the Dell Latitude 5310 from a
USB stick (after the dual-firmware boot — see `2026-06-19-dual-boot-uefi-bios-design.md`):

- **Component A — 64-bit memory map + huge-page kernel identity map (piece 3).** Fully QEMU-verifiable.
- **Component B — xHCI BIOS/firmware handoff (piece 2).** A safe no-op on QEMU (regression-verifiable);
  truly exercised only on the Dell's real xHCI.

Both are required for the live-USB boot on real hardware. Out of scope: NVMe, the e1000e/I219 NIC
driver, APIC, Secure Boot, and ext4-root-under-Limine (the boot image stays ext2 root by decision).

## 2. Component A — 64-bit memory map + huge-page identity map

**The defect.** `mmuInitKernel(FrameAllocator&, uint32_t topOfRam)` rebuilds a fresh PML4 and identity-
maps all RAM up to `topOfRam`; `bootMemTop()` returns `uint32_t` clamped to 4 GiB; and the
`markFree` callback in `kernel/Kernel.cpp` passes ranges to the pool as `(uint32_t) base, (uint32_t) len`.
Consequences on a machine with > 4 GiB RAM (the Dell has 8–16 GiB):
- RAM above 4 GiB is silently ignored (clamp), and worse,
- a usable mmap range whose base ≥ 4 GiB (e.g. `0x1_0000_0000`) **wraps** when cast to `uint32_t`
  (→ `0x0`), so the pool can mark **reserved low frames** (kernel image, page tables) as free and later
  hand them out — memory corruption / crash.

**The fix (64-bit clean, end to end):**
- `arch::bootMemTop()` returns **`uint64_t`**, clamped to the `FrameAllocator` bitmap ceiling
  (`MAX_PHYS` = 16 GiB) rather than 4 GiB. (`bootinfo_x86_64.cpp` / `MultibootMmap` already track 64-bit
  base/length; only the final clamp is 32-bit — widen it.)
- `mmuInitKernel(FrameAllocator&, uint64_t topOfRam)` takes `uint64_t`.
- The `markFree` callback (`kernel/Kernel.cpp`) passes the 64-bit `base`/`length` straight to
  `FrameAllocator::markRangeFree` (which already takes `uint64_t`), clamped to `≤ 16 GiB` (skip/trim a
  range beyond the bitmap rather than wrapping it).
- New **`AddressSpace::mapRangeHuge(va, pa, len, flags)`** — identity-map with **2 MiB PD entries**
  (the PS bit), used by `mmuInitKernel` for the all-RAM kernel map so the page-table cost stays tiny
  (16 GiB = 8192 PD entries = 16 PDs ≈ 68 KiB, vs ~32 MiB of 4 KiB PTs). 2 MiB-aligned ranges only;
  the kernel identity base/top are already 2 MiB-aligned (1 MiB load + RAM top rounded down to 2 MiB).

**Isolation / interaction.** The kernel identity map lives in the kernel's PML4/PDPT slots; per-process
user windows are in separate slots, and the privatization logic (`privatizeChild`, `adoptKernelDirectory`,
`freeUserTables`) operates only on the **user-window** PDs. Huge 2 MiB PD entries in the kernel half do
not intersect those — but the plan verifies `adoptKernelDirectory` still shares the (now huge-page)
kernel PDPT entries unchanged (it shares whole PDPT-pointer entries, oblivious to PD-leaf granularity).

**Ordering.** Frames are pooled (`g_frames.init` + `markFree`) **before** `mmuInitKernel` maps them —
already the case in `Kernel.cpp`; preserved.

**Testing.**
- Host (doctest, AddressSpace + MultibootMmap are host-tested): `mapRangeHuge` over a multi-GiB range
  asserts the PD entries carry PS + the right physical base and `translate()` resolves a high VA;
  a `>4 GiB` mmap fixture asserts `bootMemTop()` returns the real 64-bit top (capped at 16 GiB) and that
  a `base ≥ 4 GiB` usable range reaches the pool un-wrapped. New cases in `tests/test_addressspace64.cpp`
  / `tests/test_multibootmmap.cpp`.
- QEMU: boot `-m 6144` (6 GiB straddles the 3–4 GiB PCI hole → exercises a high range) and `-m 8192`;
  assert the shell is reached, `/proc/meminfo` `MemTotal` > 4 GiB, and zero faults. Wired as a smoke
  (extend `smoke-x86_64` or a new `smoke-bigmem`).

## 3. Component B — xHCI BIOS/firmware handoff

**The gap.** On real hardware the firmware/SMM owns the xHCI controller at hand-off. Before our driver
resets the controller it must take ownership, or the controller keeps signalling SMIs to firmware and
fights our driver — USB-MSC reads of the root then fail. QEMU has no SMM owner, so this is a no-op there.

**Implementation** (`arch/x86_64/drivers/xhci_x86_64.cpp`, in `xhciInit` **before** `controllerInit`):
- Walk the xHCI **Extended Capabilities** list: `HCCPARAMS1` (cap reg 0x10) bits [31:16] = xECP, a
  dword offset from the MMIO base; each cap dword has `id` at byte 0 and `next` (dword stride) at byte 1.
- Find cap **ID 1 = USB Legacy Support**. In its `USBLEGSUP` dword, bit 16 = HC BIOS Owned, bit 24 = HC
  OS Owned. Set bit 24 (claim OS ownership), then poll until bit 16 (BIOS owned) clears, with a bounded
  timeout. Then in `USBLEGCTLSTS` (`USBLEGSUP` + 4) clear the SMI-enable bits so the controller stops
  raising SMIs to firmware.
- If there is no Legacy Support cap (QEMU's xHCI), skip — a clean no-op.
- **Timeout policy:** a buggy firmware that never releases → after the bounded wait, log and proceed
  best-effort (claim ownership anyway). Never hang the boot on the handoff.

**Intel port routing.** Pre-Skylake Intel chipsets share the USB2 ports between EHCI and xHCI and need
PCI-config writes (`XUSB2PR` 0xD8, `USB3_PSSEN` 0xD0) to route ports to xHCI. The Dell (Comet Lake, 2020)
is **xHCI-only** (no EHCI since Skylake) so this is almost certainly unnecessary — included **guarded**
(only for PCI vendor `0x8086`, writing the route-to-xHCI registers, which is harmless when there is no
EHCI). If the Dell's ports still fail to enumerate, this is the documented first place to look.

**Testing.** QEMU: the handoff runs as a no-op (no Legacy cap) and the existing `smoke-usb` (root on a
USB mass-storage device) + `smoke-x86_64`/`smoke-uefi` still PASS — the regression guard. Real Dell: the
only place the handoff is genuinely exercised; that is the hardware-lab milestone, not a CI gate here.

## 4. Risks

- **Huge-page kernel map vs privatization:** mitigated by the separate-PML4-slot argument above; the plan
  adds an `adoptKernelDirectory`/fork host-test pass to confirm no regression.
- **Port routing on Comet Lake is uncertain** — USBLEGSUP is the high-confidence part; routing is a
  defensive, Intel-guarded addition whose real test is the Dell.
- **Handoff against a buggy BIOS** — bounded timeout + best-effort proceed; logged.
- **Mapping 16 GiB** still requires those frames pooled first; ordering preserved, and the huge-page map
  keeps the cost negligible.

## 5. Out of scope

NVMe / internal-disk boot, e1000e (I219) NIC, APIC/IOAPIC, Secure Boot, ext4-root-under-Limine. These
are separate follow-ups; this spec is strictly the memory-map + xHCI-handoff gaps that block a real-HW
USB boot.
