#!/usr/bin/env bash
# smoke-futexpp.sh — SMP futex lost-wakeup gate. At -smp 4, futexpp ping-pongs a token through a
# condition variable; a lost cross-CPU FUTEX_WAKE on a not-yet-BLOCKED waiter parks a thread forever
# so "futexpp: PASS" never prints. Provably-can-fail: with the pre-fix futex WAIT park (BLOCKED set
# AFTER releasing g_futexLock) this deadlocks at -smp 4. MUST be -smp 4 to expose it.
set -u
IMG=disk/image64.img
SER=/tmp/nanos-futexpp-serial.log
INT=/tmp/nanos-futexpp-int.log
MON=/tmp/nanos-futexpp-qmon.sock
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
typ("futexpp", 40.0)            # 100k cond-var handshakes across 2 threads; a lost wakeup deadlocks
s.close()
PY
sleep 2
PASS=1
chk(){ if grep -q "$1" "$SER" 2>/dev/null; then echo "  OK  : $2"; else echo "  FAIL: $2 (missing: $1)"; PASS=0; fi; }
no(){ if grep -qE "$1" "$SER" 2>/dev/null; then echo "  FAIL: $2"; PASS=0; else echo "  OK  : $2"; fi; }
echo "=== SMP futex lost-wakeup boot smoke ==="
chk "jan@nanos"                              "reached the shell"
chk "futexpp: PASS"                          "100k cond-var handshakes completed (no lost wakeup)"
no  "futexpp: FAIL"                          "no create failure"
no  "KERNEL EXCEPTION|Triple fault|PANIC"    "no kernel fault"
if grep -qE "Triple fault" "$INT" 2>/dev/null; then echo "  FAIL: triple fault in int log"; PASS=0; else echo "  OK  : no triple fault"; fi
echo "==================================="
if [ "$PASS" = 1 ]; then echo "SMP futex smoke: PASS"; exit 0; else echo "SMP futex smoke: FAIL (likely lost-wakeup deadlock)"; exit 1; fi
