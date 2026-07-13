#!/usr/bin/env bash
# smoke-memfd.sh — prove /tmp is user-writable (1777) and memfd_create yields a usable scratch fd
# (ftruncate + write/read round-trip). These unblock Node/Chromium temp files + memfd shared memory.
# Plan 01 Task 1.5 groundwork. (Cross-process MAP_SHARED writeback is separate remaining work; see
# user/shmdualtest.c.)
#
# Provably-can-fail: before /tmp became 1777, the unprivileged create failed and memfdtest exited 1.
set -u
IMG=disk/image64.img
SER=/tmp/nanos-memfd-serial.log
INT=/tmp/nanos-memfd-int.log
MON=/tmp/nanos-memfd-qmon.sock
rm -f "$SER" "$INT" "$MON"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }
pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null
qemu-system-x86_64 -cpu qemu64 -smp 4 -m 2048 -drive file="$IMG",format=raw \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -display none -serial file:"$SER" -monitor unix:"$MON",server,nowait -no-reboot -d int,cpu_reset -D "$INT" &
QPID=$!
trap 'kill -9 "$QPID" 2>/dev/null' EXIT
for i in $(seq 1 45); do grep -q "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done
python3 - "$MON" <<'PY'
import socket,time,sys
s=socket.socket(socket.AF_UNIX)
try: s.connect(sys.argv[1])
except: sys.exit(0)
time.sleep(0.3)
try: s.settimeout(0.3); s.recv(65536)
except: pass
SYM = {' ': 'spc', '/': 'slash'}
def typ(text, settle):
    for c in text:
        s.sendall(("sendkey "+SYM.get(c,c)+"\n").encode()); time.sleep(0.05)
    s.sendall(b"sendkey ret\n"); time.sleep(settle)
typ("jan", 1.5)
typ("jan", 2.5)
typ("memfdtest", 3.0)
s.close()
PY
sleep 2
PASS=1
chk(){ if grep -q "$1" "$SER" 2>/dev/null; then echo "  OK  : $2"; else echo "  FAIL: $2 (missing: $1)"; PASS=0; fi; }
no(){ if grep -qE "$1" "$SER" 2>/dev/null; then echo "  FAIL: $2"; PASS=0; else echo "  OK  : $2"; fi; }
echo "=== memfd / user-writable-tmp boot smoke ==="
chk "jan@nanos"        "reached the shell"
chk "memfdtest: PASS"  "unprivileged /tmp create + memfd ftruncate/write/read round-trip"
no  "memfdtest: FAIL"  "no memfd CHECK failures"
no  "KERNEL EXCEPTION|Page fault|General protection|PANIC" "no faults in serial"
if grep -qE "Triple fault" "$INT" 2>/dev/null; then echo "  FAIL: triple fault in int log"; PASS=0; else echo "  OK  : no triple fault"; fi
echo "================================="
if [ "$PASS" = 1 ]; then echo "memfd smoke: PASS"; exit 0; else echo "memfd smoke: FAIL"; exit 1; fi
