#!/usr/bin/env bash
# smoke-eventfd.sh — prove the event-loop primitives (eventfd2 + level-triggered epoll) added for
# Node/Chromium/Electron work end-to-end in a real NanOS boot: the userland microtests eventfdtest
# and epolltest both print PASS, with zero faults. This is plan 01 Task 1.2's QEMU gate.
#
# Provably-can-fail: point IMG at an image built before these syscalls existed (or break a CHECK in
# user/eventfdtest.c) and the "eventfdtest: PASS" marker never appears -> exit 1.
set -u
IMG=disk/image64.img
SER=/tmp/nanos-eventfd-serial.log
INT=/tmp/nanos-eventfd-int.log
MON=/tmp/nanos-eventfd-qmon.sock
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
typ("jan", 1.5)                 # username
typ("jan", 2.5)                 # password -> bash login shell
typ("eventfdtest", 3.0)         # counter/poll/dup/fork semantics
typ("epolltest", 6.0)           # epoll + a 2s cross-thread blocking wait
s.close()
PY
sleep 2
PASS=1
chk(){ if grep -q "$1" "$SER" 2>/dev/null; then echo "  OK  : $2"; else echo "  FAIL: $2 (missing: $1)"; PASS=0; fi; }
no(){ if grep -qE "$1" "$SER" 2>/dev/null; then echo "  FAIL: $2"; PASS=0; else echo "  OK  : $2"; fi; }
echo "=== eventfd/epoll boot smoke ==="
chk "jan@nanos"          "reached the shell"
chk "eventfdtest: PASS"  "eventfd2 counter/poll/dup/fork semantics"
chk "epolltest: PASS"    "level-triggered epoll (pipe+eventfd, timeout, DEL, cross-thread wake)"
no  "eventfdtest: FAIL"  "no eventfd CHECK failures"
no  "epolltest: FAIL"    "no epoll CHECK failures"
no  "CPU EXCEPTION|KERNEL EXCEPTION|Page fault|General protection|PANIC" "no faults in serial"
if grep -qE "Triple fault" "$INT" 2>/dev/null; then echo "  FAIL: triple fault in int log"; PASS=0; else echo "  OK  : no triple fault"; fi
echo "================================="
if [ "$PASS" = 1 ]; then echo "eventfd/epoll smoke: PASS"; exit 0; else echo "eventfd/epoll smoke: FAIL"; exit 1; fi
