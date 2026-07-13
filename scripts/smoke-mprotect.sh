#!/usr/bin/env bash
# smoke-mprotect.sh — prove real mprotect (the RW<->RX flip V8 needs for W^X JIT code) works in a
# live NanOS boot: the mmapexectest microtest maps RW, writes code, flips to RX, executes it, and
# verifies a write to the now read-only page faults a forked child — all PASS, no host faults.
# Plan 01 Task 1.3's QEMU gate.
#
# Provably-can-fail: with the old no-op mprotect, "write to RX page faults the child" never held, so
# mmapexectest printed FAIL and this exits 1. (Same if a CHECK in user/mmapexectest.c is broken.)
set -u
IMG=disk/image64.img
SER=/tmp/nanos-mprotect-serial.log
INT=/tmp/nanos-mprotect-int.log
MON=/tmp/nanos-mprotect-qmon.sock
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
typ("mmapexectest", 3.0)        # map RW -> write code -> flip RX -> exec -> fork write-faults
s.close()
PY
sleep 2
PASS=1
chk(){ if grep -q "$1" "$SER" 2>/dev/null; then echo "  OK  : $2"; else echo "  FAIL: $2 (missing: $1)"; PASS=0; fi; }
no(){ if grep -qE "$1" "$SER" 2>/dev/null; then echo "  FAIL: $2"; PASS=0; else echo "  OK  : $2"; fi; }
echo "=== mprotect W^X boot smoke ==="
chk "jan@nanos"                              "reached the shell"
chk "mmapexectest: PASS"                     "mmap RW -> RX flip, execute, write-to-RX faults, flip back"
no  "mmapexectest: FAIL"                     "no mprotect CHECK failures"
# The child intentionally faults writing the RX page; that is a USER fault (delivered as SIGSEGV to
# the child), which must NOT surface as a kernel/host exception. Assert the host stayed clean.
no  "KERNEL EXCEPTION|Triple fault|PANIC"    "no kernel fault (the child's write fault is user-level)"
if grep -qE "Triple fault" "$INT" 2>/dev/null; then echo "  FAIL: triple fault in int log"; PASS=0; else echo "  OK  : no triple fault"; fi
echo "================================="
if [ "$PASS" = 1 ]; then echo "mprotect W^X smoke: PASS"; exit 0; else echo "mprotect W^X smoke: FAIL"; exit 1; fi
