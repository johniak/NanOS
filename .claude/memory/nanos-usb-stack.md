---
name: nanos-usb-stack
description: "USB stack (Stream F) for x86_64 COMPLETE (11/11): live-USB read-write root + USB-HID keyboard/mouse, all in-kernel; verify64 green (633 tests + ATA smoke + live-USB smoke)"
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

USB stack for the NanOS x86_64 kernel (Stream F) — enables the **live-USB scenario** (whole system
on a pendrive, USB = root). Plan: `docs/superpowers/plans/2026-06-19-usb-stack-live-usb.md`; design:
`docs/superpowers/specs/2026-06-19-usb-stack-live-usb-design.md`. Branch `feat/x86_64-foundation`.

**DONE + verified (10 of 11 tasks):**
- `<arch/usbhc.h>` — MI↔MD host-controller contract (UsbSetup/UsbTransfer/UsbHcOps; usbHcRegister/
  usbHc/usbHcOps; `usbHostInit()` is the MD bring-up entry Kernel.cpp calls).
- MI USB core `usb/UsbCore.*` (descriptors + enumeration), HID decoders `usb/UsbHid.*` (boot kbd/mouse
  → PS/2 set-1 scancodes via `hidUsageToScancode()` switch — NOT a sparse array, GCC C++ rejects that),
  MSC `usb/UsbMsc.*` (BOT + SCSI READ CAPACITY/READ(10)/WRITE(10)), `drivers/UsbMscBlockDevice.*`.
  All host-tested via mock HC `tests/usb_mock_hc.h` (scripted descriptors + SCSI disk w/ backing store).
  633 host tests pass, modules 100% covered, aggregate ≥90%.
- MD `arch/x86_64/drivers/xhci_x86_64.cpp` — full xHCI: PCI discovery (class 0C/03/30), MMIO,
  controller init (§4.2), command+event rings+ERST, port reset, Enable Slot/Address Device/Evaluate
  Context, EP0 control + bulk/interrupt endpoints (Configure Endpoint + Normal TRB). Poll model (no IRQ).
  QEMU note: xHCI cap block only services 32-bit MMIO reads (read CAPLENGTH/HCIVERSION as one dword).
- Kernel.cpp: `usbHostInit()` before storage stack; `usbStorageDiscover()` binds MSC→UsbMscBlockDevice;
  root discovery prefers a USB volume with a valid MBR, ATA fallback retained (QEMU -drive path intact).
- `make smoke-usb` gate (scripts/smoke-usb.sh) wired into `verify64`: boots root-on-`usb-storage`,
  asserts xHCI+USB root mount+shell+ring-3 fork/exec+zero faults. **PASS.** WRITE(10) verified
  e2fsck-clean + marker persists across reboot on the USB root.

**Verify cmd:** `qemu-system-x86_64 -cpu qemu64 -m 512 -drive if=none,id=usbstick,file=disk/image64-grub2.img,format=raw -device qemu-xhci -device usb-storage,drive=usbstick ...` → "Root: USB mass-storage device (usb0)".

**Task 6 (USB-HID) DONE — in-kernel, not a kext.** `kernel/UsbHidInput.cpp` (MI): `usbHidInit()` scans
the registry for boot HID interfaces, registers a kext::MouseDevice (compiled into the kernel now) as
/dev/input<N>, and starts a poll kernel thread (`Scheduler::create(usbHidPollBody, 4)`). The thread, per
device: SET_PROTOCOL(boot) + configureEndpoint the interrupt-IN EP once, then each tick calls the new
`UsbHcOps::intPoll` (non-blocking: arms one Normal TRB, returns bytes when a report arrived, re-arms),
decodes via UsbHid, and feeds `knx_feed_scancode` (kbd → /dev/input0, same sink as PS/2) or
MouseDevice::event (new public method) EV_REL/EV_KEY/EV_SYN. NanWM unchanged. **smoke-usb types the
console fork/exec THROUGH the usb-kbd → proven.** Decided in-kernel (not the spec's kext) because the
poll thread + USB core + decoders + evdev are all already in-kernel and a poll-model interrupt EP needs
a kernel thread; kext packaging would only add a big export surface for no isolation gain (future refactor).

**Two xHCI bugs found+fixed during HID bring-up (remember for real-HW):** (1) interrupt EP context MUST
set the Interval field (dword0 bits 23:16) or QEMU never schedules periodic transfers — set ~8ms
(Interval=6). (2) Per-device DMA report buffers + DON'T memset the buffer before reading: the controller
may have already DMA'd a completed report on a prior poll iteration; clearing it loses the data.

**Deviations from the plan (intentional, sound):** xHCI bulk/interrupt EP support was implemented early
(plan put it in Task 9 Step 1) because both MSC and HID need it. UsbMscBlockDevice.readSectors returns
**0 on success** (the real BlockDevice HAL convention; Kernel.cpp checks `!=0`), not the count the plan's
example test asserted. See [[nanos-x64-migration]], [[nanos-real-hardware-roadmap]].
