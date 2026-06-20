#!/usr/bin/env bash
# smoke-e1000e.sh — boot with the 82574L (-device e1000e) instead of the default e1000, exercising the
# shared E1000Core via single-vector MSI + the new minimal LAPIC + NAPI. QEMU emulates the I219's
# QEMU-absent MSI/NAPI path through the 82574L, so this is the headless gate for that infrastructure:
# the e1000e kext binds the 82574L, publishes eth0, AND a real ping round-trips over MSI (proving RX +
# TX interrupts actually deliver through the LAPIC) — all with zero faults.
set -u
IMG=disk/image64-grub2.img
SER=/tmp/nanos-e1000e-serial.log
INT=/tmp/nanos-e1000e-int.log
MON=/tmp/nanos-e1000e-qmon.sock
rm -f "$SER" "$INT" "$MON"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }
pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null
qemu-system-x86_64 -cpu qemu64 -m 512 -drive file="$IMG",format=raw \
    -netdev user,id=n0 -device e1000e,netdev=n0 \
    -display none -serial file:"$SER" -monitor unix:"$MON",server,nowait -no-reboot -d int,cpu_reset -D "$INT" &
QPID=$!
trap 'kill -9 "$QPID" 2>/dev/null' EXIT
for i in $(seq 1 45); do grep -q "bash-5" "$SER" 2>/dev/null && break; sleep 1; done
# Drive a ping to the QEMU gateway (10.0.2.2) through the monitor — exercises RX+TX over MSI.
python3 - "$MON" <<'PY'
import socket,time,sys
s=socket.socket(socket.AF_UNIX)
try: s.connect(sys.argv[1])
except: sys.exit(0)
time.sleep(0.3)
SYM={' ':'spc','.':'dot','-':'minus'}
for c in "ping -c 2 10.0.2.2":
    s.sendall(("sendkey "+SYM.get(c,c)+"\n").encode()); time.sleep(0.05)
s.sendall(b"sendkey ret\n"); time.sleep(6)
s.close()
PY
sleep 1
PASS=1
chk(){ if grep -q "$1" "$SER" 2>/dev/null; then echo "  OK  : $2"; else echo "  FAIL: $2 (missing: $1)"; PASS=0; fi; }
no(){ if grep -qE "$1" "$SER" 2>/dev/null; then echo "  FAIL: $2"; PASS=0; else echo "  OK  : $2"; fi; }
echo "=== e1000e (82574L) MSI boot smoke ==="
chk "bash-5"           "reached the shell"
chk "e1000e: eth0 up"  "e1000e bound the 82574L and published eth0 (shared core + MSI + LAPIC)"
chk "2 packets received" "ping round-trip over MSI (RX + TX interrupts deliver through the LAPIC)"
no  "CPU EXCEPTION|KERNEL EXCEPTION|Triple fault" "no faults on the console"
if grep -qE "Triple fault" "$INT" 2>/dev/null; then echo "  FAIL: triple fault in int log"; PASS=0; else echo "  OK  : no triple fault"; fi
echo "======================================"
if [ "$PASS" = 1 ]; then echo "e1000e MSI boot smoke: PASS"; exit 0; else echo "e1000e MSI boot smoke: FAIL"; exit 1; fi
