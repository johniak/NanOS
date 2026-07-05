# NanOS USB stack (Stream F) + live-USB root — design

**Date:** 2026-06-19
**Branch:** `feat/x86_64-foundation` (x86_64 is the sole development arch; i686 frozen — see
`docs/superpowers/ROADMAP.md` / the migration status doc).
**Status:** design approved, ready for an implementation plan.

---

## 1. Goal

A USB stack for the NanOS x86_64 kernel that supports the **live-USB scenario**: the *entire*
system — kernel, kernel modules (kexts), and the root filesystem — lives on a USB pendrive, booted by
GRUB. Concretely:

- **xHCI** host controller (USB 3.x, backward-compatible with USB 2/1.1 devices; the only controller —
  no legacy UHCI/EHCI/OHCI). Target real HW = Dell Latitude 5310 (xHCI); QEMU `-device qemu-xhci`.
- **USB core**: descriptor parsing, device enumeration (reset → address → config), transfer plumbing.
- **USB Mass Storage Class (MSC)**: Bulk-Only Transport (BOT) + SCSI `READ CAPACITY(10)` / `READ(10)`
  → a `BlockDevice` → mountable as a `/disks/<name>` volume. **Read-only in phase 1**; `WRITE(10)` is a
  follow-on phase.
- **USB HID**: boot-protocol keyboard + mouse → the *existing* evdev input devices (`/dev/input0` etc.)
  → consumed by NanWM unchanged.

Success = on QEMU, the kernel boots with the root filesystem on an emulated USB mass-storage device
(`-device qemu-xhci -device usb-storage,drive=...`), mounts `/disks/main` from it, runs `init.nxe`,
and a USB keyboard/mouse (`usb-kbd`/`usb-mouse`) drives the NanWM desktop — zero faults.

## 2. The live-USB constraint (the decision that shapes everything)

When the whole system is on the pendrive, **USB is the ROOT storage**. The kernel must read the
pendrive's filesystem *after* GRUB hands off (firmware/BIOS USB support is gone in long mode), so it
needs a USB mass-storage driver — but that driver cannot itself be a kext stored on the pendrive (no
way to read it). This is the same bootstrap rule NanOS already follows for ATA:

> **Principle: the driver for the ROOT storage is built INTO the kernel.** Today root=disk →
> `AtaBlockDevice` is in-kernel (`arch/x86_64/drivers/`, registered in `Kernel.cpp`, NOT a kext).
> For a USB root, the USB **storage path** (xHCI + USB-core + MSC) is likewise **in-kernel**.

Everything not needed to reach the root stays a loadable **kext**, read from the now-mounted root —
consistent with the existing e1000 / PS-2 model. Keyboard/mouse are not needed to mount root, so
**USB-HID is a kext**.

This avoids any initramfs / Multiboot-module kext-loading machinery: GRUB just loads the kernel; the
in-kernel USB storage path mounts the USB root; the rest loads from there.

| Component | Packaging | Why |
|---|---|---|
| xHCI HC driver | **in-kernel** (`arch/x86_64`) | root storage path |
| USB core (enumeration/descriptors/transfers) | **in-kernel** (MI) | needed by MSC at boot |
| USB MSC (BOT + SCSI read) | **in-kernel** (MI) | exposes the root BlockDevice |
| USB HID (kbd/mouse) | **kext** (`kext/usbhid/`) | not root-critical; loads from mounted root |

## 3. Architecture (MI/MD split, follows existing contracts)

```
MI — host-testable (doctest):                     MD — arch/x86_64 (QEMU-tested):
  usb/UsbCore.*   enumeration state machine,         arch/x86_64/drivers/xhci_x86_64.*
                  descriptor parse, transfer            xHCI: MMIO regs, command/event/
                  request objects, device model         transfer rings, ERST, slot/EP
  usb/UsbMsc.*    BOT state machine + SCSI CDBs         contexts, DMA (identity-mapped
                  (READ CAPACITY/READ(10)) ->           frames; no IOMMU), MSI/legacy IRQ.
                  drivers/UsbMscBlockDevice (HAL)        Implements <arch/usbhc.h>.
       │  submit(transfer) / completion(cb)                     │
       └──────────  <arch/usbhc.h> contract  ──────────────────┘
                    (host-controller: submit URB-like
                     transfer, register completion;
                     MI core is HC-agnostic)

  kext/usbhid/  USB-HID class: parse HID report descriptor, boot-protocol
                kbd -> kbdFeed()/KeyboardDevice (/dev/input0);
                mouse -> EV_REL/EV_KEY/EV_SYN -> MouseDevice (/dev/input<N>).
                Registered via knx_add_input_dev — NanWM unchanged.
```

**New MI↔MD contract `<arch/usbhc.h>`** (mirrors `<arch/block.h>`): the MD xHCI driver registers as a
USB *host controller* offering `submitTransfer(slot, endpoint, buffer, len, dir, completion)` and
port/event notifications; MI `UsbCore` drives enumeration and class logic through it, controller-
agnostic. A second arch (later) implements the same contract.

**Input integration (hard requirement, per NanWM):** USB-HID creates NO new input path. It feeds the
existing MI devices: keyboard via `kbdFeed()` → `KeyboardDevice` = `/dev/input0`; mouse by emitting
`EV_REL/EV_KEY/EV_SYN` into a `MouseDevice` registered via `knx_add_input_dev` (a `/dev/input<N>`). PS-2
and USB coexist as producers; the kernel console and **NanWM consume the same buffers — no NWM change.**
USB-HID translates HID usage codes → the keycode/scancode format `KeyboardDevice` expects.

## 4. Boot / root-discovery flow

```
Firmware boots GRUB from USB (firmware's own USB) → GRUB loads the NanOS kernel (Multiboot) into RAM.
Kernel::start:
  1. (existing) GDT/IDT/paging/console.
  2. NEW: bring up in-kernel USB storage path — init xHCI (PCI-discovered), USB-core enumerate,
     bind USB-MSC to any mass-storage device -> register a UsbMscBlockDevice with DeviceManager.
  3. Root discovery: pick the boot volume — try the USB-MSC device(s) with a valid NanOS partition
     (MBR/GPT probe, like the current ATA path); fall back to AtaBlockDevice (the QEMU -drive image
     case). Mount it at /disks/main.
  4. (existing) loadAllKexts("/disks/main/nanos/kext") — now from the USB root — loads USB-HID,
     e1000, PS-2, etc.
  5. (existing) exec /disks/main/nanos/core/init.nxe.
```

GRUB-from-USB itself: the current `image64.img` `dd`-ed to a pendrive is BIOS/CSM-bootable as-is
(GRUB2). Pure-UEFI USB boot (ESP partition) is a Stream-D refinement, out of scope here; the *kernel-
side USB-root mount* is the substance of this plan and works under either firmware path.

## 5. DMA / memory model

xHCI needs physically-contiguous, identity-addressable memory for its rings + device contexts. NanOS
kernel is identity-mapped with no IOMMU, so a frame's physical address == its kernel virtual address;
ring/context buffers come from `FrameAllocator` (page-aligned). 64-bit ring pointers are native LP64.
(Reuse the same frame source the kernel already uses; no new allocator.)

## 6. Error handling

- Enumeration timeout / STALL → reset endpoint, retry bounded; log + skip the device (never panic on a
  bad device — a CPU-fault would, but a protocol error must degrade).
- MSC: BOT CSW failure / SCSI CHECK CONDITION → REQUEST SENSE, retry bounded, then surface `-EIO` from
  `readSectors` so the VFS/ext layer reports an error rather than corrupting.
- No USB mass-storage device with a NanOS partition found AND no ATA fallback → clear panic message
  ("no bootable root") rather than a silent hang.
- Hot-unplug of the root device is out of scope (treated as fatal I/O error, like pulling a disk).

## 7. Testing

- **MI host doctests** (`make ARCH=x86_64 test`, ≥90% gated): descriptor parser, enumeration state
  machine driven by a **mock host controller** (in-RAM, scripts device responses), HID report-
  descriptor parser + boot-kbd/mouse → evdev event assertions, BOT/SCSI CDB encode + CSW decode,
  `UsbMscBlockDevice.readSectors` over the mock. New `tests/test_usb*.cpp`; modules added to
  `TEST_MODULES`/`COV_PATTERNS`. `make check-arch` stays clean (USB core is MI).
- **QEMU e2e** (extend `scripts/smoke-x86_64.sh` / a new `smoke-usb` target): boot with
  `-device qemu-xhci -device usb-storage,drive=root` (root FS on USB) → asserts mount + `init` +
  zero faults; `-device usb-kbd -device usb-mouse` → screendump shows NanWM taking USB input.

## 8. Phasing (becomes the plan's task groups)

1. **`<arch/usbhc.h>` + xHCI bring-up (MD):** PCI-discover xHCI, MMIO init, command/event rings, port
   reset; enumerate a device to "addressed". QEMU: xHCI present, a `usb-kbd` reaches address state.
2. **USB core (MI):** descriptor parsing, full enumeration (config/interface/endpoint), control/bulk/
   interrupt transfer requests over the HC contract. Host-tested on the mock HC.
3. **MSC + USB root (in-kernel):** BOT + SCSI read → `UsbMscBlockDevice` → DeviceManager → root
   discovery + mount `/disks/main` from USB. QEMU: boot with root on `usb-storage`.
4. **USB-HID kext:** HID parser + boot kbd/mouse → existing evdev → NanWM. QEMU: USB mouse/kbd in NWM.
5. **(follow-on) SCSI `WRITE(10)`** → read-write USB root; e2fsck-clean after writes.

## 9. Decisions, constraints, risks

- **Decisions:** xHCI-only; storage path in-kernel (root rule), HID as kext; MSC read-only first;
  `<arch/usbhc.h>` contract; no IOMMU (identity DMA).
- **Constraints:** HID must feed the existing `/dev/input*` evdev (NanWM unchanged); USB core stays MI
  (`check-arch`); root-storage driver stays in-kernel.
- **Risks:** (a) xHCI ring/IRQ bring-up is the hardest MD piece — mitigate with the mock-HC MI tests +
  QEMU before any hardware; (b) root-discovery ordering (USB vs ATA) — keep ATA fallback so the QEMU
  disk-image flow is unaffected; (c) kernel size grows by the in-kernel USB storage path — accepted
  (same trade-off as in-kernel ATA); (d) real-HW xHCI quirks vs QEMU — deferred to the hardware-lab
  phase, this plan targets QEMU.
- **Out of scope:** UEFI/ESP USB boot (Stream D), USB hubs beyond one tier (nice-to-have), USB audio/
  other classes, hot-plug of the root, IOMMU.
