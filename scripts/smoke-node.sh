#!/usr/bin/env bash
# smoke-node.sh — boot NanOS and run the Node acceptance oracle (plan 02 Task 2.6). Proves node.nxe
# starts, executes JS, and that fs/net/dns/timers/promises work: node-smoke.js prints "node-smoke:
# PASS". Provably-can-fail: before node.nxe existed the run printed no PASS marker.
set -u
IMG=disk/image64.img
SER=/tmp/nanos-node-serial.log
INT=/tmp/nanos-node-int.log
MON=/tmp/nanos-node-qmon.sock
rm -f "$SER" "$INT" "$MON"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }
pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null
# 3 GiB: V8 reserves a large heap/code cage; the default 2 GiB is tight.
qemu-system-x86_64 -cpu qemu64 -smp 4 -m 3072 -drive file="$IMG",format=raw \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -display none -serial file:"$SER" -monitor unix:"$MON",server,nowait -no-reboot -d int,cpu_reset -D "$INT" &
QPID=$!
trap 'kill -9 "$QPID" 2>/dev/null' EXIT
for i in $(seq 1 60); do grep -q "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done
python3 - "$MON" <<'PY'
import socket,time,sys
s=socket.socket(socket.AF_UNIX)
try: s.connect(sys.argv[1])
except: sys.exit(0)
time.sleep(0.3)
try: s.settimeout(0.3); s.recv(65536)
except: pass
SYM = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus'}
def typ(text, settle):
    for c in text:
        s.sendall(("sendkey "+SYM.get(c,c)+"\n").encode()); time.sleep(0.05)
    s.sendall(b"sendkey ret\n"); time.sleep(settle)
typ("jan", 1.5)
typ("jan", 2.5)
# node startup + JS execution can take a few seconds under TCG; give it room.
typ("node /apps/node-smoke/node-smoke.js", 12.0)
s.close()
PY
sleep 2
PASS=1
chk(){ if grep -q "$1" "$SER" 2>/dev/null; then echo "  OK  : $2"; else echo "  FAIL: $2 (missing: $1)"; PASS=0; fi; }
no(){ if grep -qE "$1" "$SER" 2>/dev/null; then echo "  FAIL: $2"; PASS=0; else echo "  OK  : $2"; fi; }
echo "=== Node boot smoke ==="
chk "jan@nanos"       "reached the shell"
chk "node-smoke: PASS" "node runs JS + fs/net/dns/timers/promises"
no  "node-smoke: FAIL"  "no node-smoke CHECK failures"
no  "KERNEL EXCEPTION|Page fault|General protection|PANIC" "no faults in serial"
if grep -qE "Triple fault" "$INT" 2>/dev/null; then echo "  FAIL: triple fault in int log"; PASS=0; else echo "  OK  : no triple fault"; fi
echo "================================="
if [ "$PASS" = 1 ]; then echo "node smoke: PASS"; exit 0; else echo "node smoke: FAIL"; exit 1; fi
