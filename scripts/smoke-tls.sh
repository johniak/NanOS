#!/usr/bin/env bash
# smoke-tls.sh — prove per-thread TLS works on WORKER threads (pthread_create's __copy_tls path).
# tlstest spawns 8 threads with __thread vars that have NON-ZERO initializers and checks each worker
# reads its initializer (not 0) and keeps its own private copy under contention. This is the exact
# contract V8/node needs — a worker reading 0 for V8's assert-scope thread_local aborts node at
# `Check failed: AllowHeapAllocationInRelease::IsAllowed()`.
#
# Provably-can-fail: with the old bare-TCB pthread_create (no per-thread TLS block), workers read
# garbage/0 for __thread vars, so tlstest prints FAIL and this exits 1.
set -u
IMG=disk/image64.img
SER=/tmp/nanos-tls-serial.log
INT=/tmp/nanos-tls-int.log
MON=/tmp/nanos-tls-qmon.sock
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
def typ(text, settle):
    for c in text:
        s.sendall(("sendkey "+{' ':'spc'}.get(c,c)+"\n").encode()); time.sleep(0.05)
    s.sendall(b"sendkey ret\n"); time.sleep(settle)
typ("jan", 1.5)                 # username
typ("jan", 2.5)                 # password -> bash login shell
typ("tlstest", 4.0)             # 8 threads x per-thread TLS init + privacy checks
s.close()
PY
sleep 2
PASS=1
chk(){ if grep -q "$1" "$SER" 2>/dev/null; then echo "  OK  : $2"; else echo "  FAIL: $2 (missing: $1)"; PASS=0; fi; }
no(){ if grep -qE "$1" "$SER" 2>/dev/null; then echo "  FAIL: $2"; PASS=0; else echo "  OK  : $2"; fi; }
echo "=== worker-thread TLS boot smoke ==="
chk "jan@nanos"                              "reached the shell"
chk "tlstest: PASS"                          "8 workers read __thread initializers + keep private TLS"
no  "tlstest: FAIL"                          "no TLS CHECK failures"
no  "KERNEL EXCEPTION|Triple fault|PANIC"    "no kernel fault"
if grep -qE "Triple fault" "$INT" 2>/dev/null; then echo "  FAIL: triple fault in int log"; PASS=0; else echo "  OK  : no triple fault"; fi
echo "==================================="
if [ "$PASS" = 1 ]; then echo "worker-thread TLS smoke: PASS"; exit 0; else echo "worker-thread TLS smoke: FAIL"; exit 1; fi
