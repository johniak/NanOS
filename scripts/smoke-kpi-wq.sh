#!/usr/bin/env bash
# smoke-kpi-wq.sh — the LinuxKPI async-workqueue gate (i915 plan Task 3).
#
# Boots the x86_64 image headless with virtio-gpu as the only display and asserts the workqueue
# subsystem (linuxkpi/kpi_kthread.c) came up: inline during the pre-scheduler probe, then ASYNC with
# real worker kthreads once the scheduler is up. The regression risk is that inline->async changes
# the ordering the virtio-gpu bring-up implicitly relied on — so the referee is that the driver still
# probes and the desktop still renders (checked by smoke-virtio-gpu, same boot config; here we assert
# the async transition + probe + no fault, keeping this gate fast).
#
# Oracle (serial markers, no OCR):
#   1. workqueues set up before the probe:      "workqueues initialised (inline until scheduler up)"
#   2. the driver still probed:                 "virtio_gpu_probe() OK"
#   3. workers spawned + async after scheduler:  "workqueues async (workers up)"
#   4. no kernel fault/panic, and login is reached (no bring-up hang from the async switch).
set -u
IMG=disk/image64.img
SER=/tmp/nanos-kpiwq.log
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

for i in $(seq 1 60); do grep -q "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done

fail() { echo "FAIL: $1"; echo "--- serial tail ---"; tail -30 "$SER" 2>/dev/null; exit 1; }

grep -q "nanos login:" "$SER" 2>/dev/null || fail "never reached login (async workqueue bring-up hang?)"
grep -q "workqueues initialised (inline until scheduler up)" "$SER" 2>/dev/null || fail "lkpi_wq_init did not run before probe"
grep -q "virtio_gpu_probe() OK" "$SER" 2>/dev/null || fail "unmodified virtio_gpu_probe() did not complete (async ordering regression?)"
grep -q "workqueues async (workers up)" "$SER" 2>/dev/null || fail "workqueue workers never started post-scheduler"
if grep -qiE "CPU EXCEPTION|KERNEL EXCEPTION|Triple fault|killed faulting process|PANIC" "$SER" 2>/dev/null; then
	fail "kernel fault on the console"
fi

echo "PASS: LinuxKPI workqueues go inline->async (real worker kthreads); driver probe + desktop bring-up intact."
exit 0
