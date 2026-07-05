#!/usr/bin/env bash
# smoke-smp.sh — the multicore boot smoke. Boots the x86_64 disk image under QEMU with FOUR
# vCPUs and asserts the SMP bring-up path came up clean: ACPI enumerated all 4 CPUs, the AP
# trampoline + INIT-SIPI-SIPI brought every application processor online, and NOTHING triple-
# faulted (the real-mode->long-mode trampoline is the fiddly part — a bad GDT/CR3 shows as a
# v=0d/v=08 in the QEMU int log even if the console says nothing). One reproducible command.
#
# Usage: scripts/smoke-smp.sh           (boots disk/image64.img; build it first)
# Exit 0 = PASS, non-zero = FAIL (with the offending serial/int-log lines printed).
set -u
IMG=disk/image64.img
SER=/tmp/nanos-smpsmoke-serial.log
INT=/tmp/nanos-smpsmoke-int.log
MON=/tmp/nanos-smpsmoke-qmon.sock
NCPU=4
rm -f "$SER" "$INT" "$MON"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }

pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null
qemu-system-x86_64 -cpu qemu64 -smp "$NCPU" -m 512 -drive file="$IMG",format=raw \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -display none -serial file:"$SER" -monitor unix:"$MON",server,nowait \
    -no-reboot -d int,cpu_reset -D "$INT" &
QPID=$!
cleanup() { kill -9 "$QPID" 2>/dev/null; }
trap cleanup EXIT

# Wait for the login prompt — it appears AFTER the SMP bring-up line (smpInit runs just before
# the scheduler starts), so reaching login proves both that the APs came online AND that the BSP
# kept booting cleanly through to init->login. APs idle (hlt) in Phase 2, so the BSP path is
# unchanged and login arrives at the usual time.
for i in $(seq 1 45); do grep -q "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done
sleep 1

# ---- assertions -------------------------------------------------------------------------------
PASS=1
chk() { if grep -q "$1" "$SER" 2>/dev/null; then echo "  OK  : $2"; else echo "  FAIL: $2 (missing: $1)"; PASS=0; fi; }
no()  { if grep -qE "$1" "$SER" 2>/dev/null; then echo "  FAIL: $2"; grep -E "$1" "$SER" | head -3 | sed 's/^/        /'; PASS=0; else echo "  OK  : $2"; fi; }

echo "=== x86_64 SMP ($NCPU vCPU) boot smoke ==="
chk "SMP: $NCPU CPUs online"                  "all $NCPU CPUs online (ACPI MADT + AP trampoline + INIT-SIPI-SIPI)"
chk "nanos login:"                            "reached login (scheduler still healthy after bring-up)"
no  "CPU EXCEPTION|KERNEL EXCEPTION|killed faulting process|Triple fault"  "no faults on the console"
# A bad AP trampoline (real-mode->long-mode) triple-faults that vCPU — caught in the int log even
# if the console is silent. cpu_reset logging makes a guest reset visible too.
if grep -qE "Triple fault" "$INT" 2>/dev/null; then echo "  FAIL: QEMU logged a Triple fault"; PASS=0; else echo "  OK  : no triple fault in QEMU int log"; fi

echo "==============================================="
if [ "$PASS" = 1 ]; then echo "x86_64 SMP smoke: PASS"; exit 0; else echo "x86_64 SMP smoke: FAIL"; exit 1; fi
