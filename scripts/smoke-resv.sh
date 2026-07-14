#!/usr/bin/env bash
# smoke-resv.sh — prove NanOS's reserve-without-backing memory model in a live boot: resvtest
# reserves 128 MiB PROT_NONE+MAP_NORESERVE (bigger than the whole 64 MiB anon window) WITHOUT
# consuming RAM, commits a 64 KiB segment via mprotect, proves uncommitted pages fault, decommits via
# a MAP_FIXED PROT_NONE over-map, and re-commits — all PASS, no host faults. This is V8's
# SegmentedTable contract; it's what unblocks node's JS eval.
#
# Provably-can-fail: with the old eager-backing model, mmap(PROT_NONE) fell into the 64 MiB anon
# window and a 128 MiB request returned MAP_FAILED, so resvtest prints FAIL and this exits 1.
set -u
IMG=disk/image64.img
SER=/tmp/nanos-resv-serial.log
INT=/tmp/nanos-resv-int.log
MON=/tmp/nanos-resv-qmon.sock
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
typ("resvtest", 4.0)            # reserve -> commit -> fault-check -> decommit -> re-commit
s.close()
PY
sleep 2
PASS=1
chk(){ if grep -q "$1" "$SER" 2>/dev/null; then echo "  OK  : $2"; else echo "  FAIL: $2 (missing: $1)"; PASS=0; fi; }
no(){ if grep -qE "$1" "$SER" 2>/dev/null; then echo "  FAIL: $2"; PASS=0; else echo "  OK  : $2"; fi; }
echo "=== reserve/commit boot smoke ==="
chk "jan@nanos"                              "reached the shell"
chk "resvtest: PASS"                         "reserve 128 MiB unbacked, commit/decommit segments, guard faults"
no  "resvtest: FAIL"                         "no reserve/commit CHECK failures"
# resvtest intentionally faults reading uncommitted/decommitted pages in a forked CHILD; those are
# USER faults (SIGSEGV to the child) and must NOT surface as a kernel/host exception.
no  "KERNEL EXCEPTION|Triple fault|PANIC"    "no kernel fault (child guard-page faults are user-level)"
if grep -qE "Triple fault" "$INT" 2>/dev/null; then echo "  FAIL: triple fault in int log"; PASS=0; else echo "  OK  : no triple fault"; fi
echo "================================="
if [ "$PASS" = 1 ]; then echo "reserve/commit smoke: PASS"; exit 0; else echo "reserve/commit smoke: FAIL"; exit 1; fi
