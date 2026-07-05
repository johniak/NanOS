#!/usr/bin/env bash
# smoke-e1000e.sh — boot with the 82574L (-device e1000e) instead of the default e1000, exercising the
# shared E1000Core via single-vector MSI + the new minimal LAPIC + NAPI. QEMU emulates the I219's
# QEMU-absent MSI/NAPI path through the 82574L, so this is the headless gate for that infrastructure:
# the e1000e kext binds the 82574L, publishes eth0, AND a real ping round-trips over MSI (proving RX +
# TX interrupts actually deliver through the LAPIC) — all with zero faults.
set -u
IMG=disk/image64.img
SER=/tmp/nanos-e1000e-serial.log
INT=/tmp/nanos-e1000e-int.log
MON=/tmp/nanos-e1000e-qmon.sock
rm -f "$SER" "$INT" "$MON"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }
pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null
# No `-d int` here: the 82574L's MSI + NAPI RX path floods the interrupt log and crawls the guest,
# so bash startup + the ping would not finish in the window. The ping ROUND-TRIP itself proves the
# MSI RX+TX interrupts deliver; faults are caught on the serial console (a triple fault would also
# reset the guest under -no-reboot, leaving the ping unanswered -> FAIL).
qemu-system-x86_64 -cpu qemu64 -m 512 -drive file="$IMG",format=raw \
    -netdev user,id=n0 -device e1000e,netdev=n0 \
    -display none -serial file:"$SER" -monitor unix:"$MON",server,nowait -no-reboot &
QPID=$!
trap 'kill -9 "$QPID" 2>/dev/null' EXIT
for i in $(seq 1 60); do grep -q "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done
# Log in (mandatory toybox login), then ping the QEMU gateway (10.0.2.2) — exercises RX+TX over MSI.
python3 - "$MON" <<'PY'
import socket,time,sys
s=socket.socket(socket.AF_UNIX)
try: s.connect(sys.argv[1])
except: sys.exit(0)
time.sleep(0.3)
try: s.settimeout(0.3); s.recv(65536)
except: pass
SYM={' ':'spc','.':'dot','-':'minus','/':'slash'}
def typ(text, settle):
    for c in text:
        s.sendall(("sendkey "+SYM.get(c,c)+"\n").encode()); time.sleep(0.05)
    s.sendall(b"sendkey ret\n"); time.sleep(settle)
typ("jan", 1.5)                       # username
typ("jan", 2.5)                       # password -> bash login shell
# ping needs root (raw socket; ping.nxe is also mode 0644 so only uid 0 may exec it). jan is in
# wheel = NOPASSWD sudo, so no password prompt. Use the FULL path WITH .nxe: sudo's execvp does not
# append .nxe the way the shell does, and secure_path only lists the bin dirs, not this file.
typ("sudo /disks/main/nanos/bin/ping.nxe -c 2 10.0.2.2", 7.0)
s.close()
PY
sleep 1
PASS=1
chk(){ if grep -q "$1" "$SER" 2>/dev/null; then echo "  OK  : $2"; else echo "  FAIL: $2 (missing: $1)"; PASS=0; fi; }
no(){ if grep -qE "$1" "$SER" 2>/dev/null; then echo "  FAIL: $2"; PASS=0; else echo "  OK  : $2"; fi; }
echo "=== e1000e (82574L) MSI boot smoke ==="
chk "jan@nanos"        "reached the shell"
chk "e1000e: eth0 up"  "e1000e bound the 82574L and published eth0 (shared core + MSI + LAPIC)"
chk "2 packets received" "ping round-trip over MSI (RX + TX interrupts deliver through the LAPIC)"
no  "CPU EXCEPTION|KERNEL EXCEPTION|Triple fault" "no faults on the console"
echo "======================================"
if [ "$PASS" = 1 ]; then echo "e1000e MSI boot smoke: PASS"; exit 0; else echo "e1000e MSI boot smoke: FAIL"; exit 1; fi
