#!/usr/bin/env bash
# smoke-usb-dmawindow.sh — regression gate for the xHCI-DMA-under-user-CR3 fault.
#
# The xHCI rings/contexts are kernel frames the CPU touches via phys==virt. A per-process address
# space privatizes the user-window VA range [8 MiB, 64 MiB), so if a ring frame lands there a USB
# transfer issued from a user process (a file read off the USB root) faults (#PF in ringPush, cr2
# in the window) — exactly the real-hardware crash on the Dell. xhciSubmit/intPoll/configureEndpoint
# run under the kernel CR3 (KernelCr3 guard) to make the full identity map active regardless of the
# caller's CR3, which fixes it.
#
# This gate builds the kernel with -DXHCI_TEST_FORCE_WINDOW, which DELIBERATELY allocates the xHCI
# endpoint ring inside [8 MiB, 64 MiB) (the dangerous range) so the fault condition is forced in
# QEMU (where rings otherwise land safely). It then boots root-on-USB + SMP and asserts the system
# reaches login with ZERO kernel exceptions. Without the KernelCr3 guard this build faults in
# ringPush; with it, boot is clean — so this catches any regression of the guard.
set -u
IMG=disk/image64-grub2.img
SER=/tmp/nanos-usbdmawin-serial.log
rm -f "$SER"
pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null

echo "=== building kernel with xHCI ring forced into the user-window range ==="
make image64 KEXTRA=-DXHCI_TEST_FORCE_WINDOW >/dev/null 2>&1 || { echo "FAIL: build error"; exit 2; }

qemu-system-x86_64 -cpu qemu64 -smp 2 -m 512 \
    -drive if=none,id=usbstick,file="$IMG",format=raw \
    -device qemu-xhci -device usb-storage,drive=usbstick -device usb-kbd -device usb-mouse \
    -display none -serial file:"$SER" -no-reboot &
QPID=$!
trap 'kill -9 "$QPID" 2>/dev/null; make image64 >/dev/null 2>&1' EXIT   # rebuild a clean image on exit
for i in $(seq 1 40); do grep -q "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done

PASS=1
echo "=== xHCI DMA-window regression (ring forced into [8MiB,64MiB), root-on-USB + -smp 2) ==="
if grep -q "nanos login:" "$SER" 2>/dev/null; then echo "  OK  : reached login with the ring in the user-window range"; else echo "  FAIL: never reached login"; PASS=0; fi
if grep -qE "KERNEL EXCEPTION|vec=0e" "$SER" 2>/dev/null; then
    echo "  FAIL: kernel exception (the CR3 guard regressed — USB DMA faulted under a user CR3)"
    grep -aE "KERNEL EXCEPTION|vec=0e" "$SER" | head -1 | sed 's/^/        /'; PASS=0
else echo "  OK  : no kernel exception (xHCI ran under the kernel CR3 despite the in-window ring)"; fi
echo "==================================================================================="
if [ "$PASS" = 1 ]; then echo "xHCI DMA-window regression: PASS"; exit 0; else echo "xHCI DMA-window regression: FAIL"; exit 1; fi
