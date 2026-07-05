#!/usr/bin/env bash
# smoke-kpi-irq.sh — the LinuxKPI real-interrupt gate (i915 plan Task 2).
#
# Boots the x86_64 image headless with the virtio-gpu device as the ONLY display and asserts that
# the device's interrupt runs through the REAL request_irq -> MSI path (not just the cooperative
# poll). QEMU's virtio-pci exposes MSI-X only, so this exercises the single-vector MSI-X programming
# added to kernel/MsiRouter.cpp plus linuxkpi/kpi_irq.c (request_irq over knx_register_msi) and the
# transport wiring (kext/virtio_gpu/virtio_transport.c vt_enable_msi).
#
# Oracle (serial markers, no OCR):
#   1. the unmodified driver still probes:                 "virtio_gpu_probe() OK"
#   2. an irq was bound to the device's MSI:               "lkpi: irq N bound (msi)"
#   3. the device's MSI-X was routed to that vector:       "MSI-X routed (ctrl vq -> vector 0)"
#   4. a real MSI reached the request_irq handler:         "lkpi: irq fired>0"
#   5. no kernel fault/panic.
# The desktop-renders half is covered by smoke-virtio-gpu (same boot config); this gate is focused
# on the interrupt path so it stays fast and does not repeat the login/screendump dance.
set -u
IMG=disk/image64.img
SER=/tmp/nanos-kpiirq.log
rm -f "$SER"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }

pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null
sleep 1
qemu-system-x86_64 -accel tcg,thread=multi -cpu qemu64 -smp 1 -m 512 -drive file="$IMG",format=raw \
    -vga none -device virtio-gpu-pci \
    -display none -serial file:"$SER" -no-reboot &
QPID=$!
cleanup() { kill -9 "$QPID" 2>/dev/null; }
trap cleanup EXIT

# The present thread starts issuing ctrl-vq commands right after bring-up, so the first MSI arrives
# within a few seconds of login being reached. Wait for the "irq fired" marker (bounded).
for i in $(seq 1 60); do grep -q "lkpi: irq fired" "$SER" 2>/dev/null && break; sleep 1; done

fail() { echo "FAIL: $1"; echo "--- serial tail ---"; tail -30 "$SER" 2>/dev/null; exit 1; }

grep -q "virtio_gpu_probe() OK" "$SER" 2>/dev/null || fail "unmodified virtio_gpu_probe() did not complete"
grep -qE "lkpi: irq [0-9]+ bound \(msi\)" "$SER" 2>/dev/null || fail "request_irq never bound the device MSI"
grep -q "MSI-X routed (ctrl vq -> vector 0)" "$SER" 2>/dev/null || fail "virtio MSI-X vector routing did not take"
grep -q "lkpi: irq fired>0" "$SER" 2>/dev/null || fail "no MSI reached the request_irq handler (interrupt not delivered)"
if grep -qiE "CPU EXCEPTION|KERNEL EXCEPTION|Triple fault|killed faulting process|PANIC" "$SER" 2>/dev/null; then
	fail "kernel fault on the console"
fi

echo "PASS: LinuxKPI request_irq -> MSI-X delivers a real interrupt from virtio-gpu (poll stays watchdog)."
exit 0
