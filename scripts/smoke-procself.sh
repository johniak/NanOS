#!/usr/bin/env bash
# smoke-procself.sh — prove /proc/self introspection works in a live boot: readlink /proc/self/exe
# names the running binary, /proc/self/fd lists an open fd, and chdir/getcwd round-trips. Node and
# Electron resolve their own executable and scan open fds through exactly these. Plan 01 Task 1.4.
#
# Provably-can-fail: before this task /proc/self/exe and /proc/self/fd did not exist (classifyProc
# rejected "self"), so procselftest printed FAIL and this exits 1.
set -u
IMG=disk/image64.img
SER=/tmp/nanos-procself-serial.log
INT=/tmp/nanos-procself-int.log
MON=/tmp/nanos-procself-qmon.sock
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
typ("procselftest", 3.0)        # /proc/self/exe readlink + /proc/self/fd listing + cwd round-trip
s.close()
PY
sleep 2
PASS=1
chk(){ if grep -q "$1" "$SER" 2>/dev/null; then echo "  OK  : $2"; else echo "  FAIL: $2 (missing: $1)"; PASS=0; fi; }
no(){ if grep -qE "$1" "$SER" 2>/dev/null; then echo "  FAIL: $2"; PASS=0; else echo "  OK  : $2"; fi; }
echo "=== /proc/self boot smoke ==="
chk "jan@nanos"          "reached the shell"
chk "procselftest: PASS" "/proc/self/exe readlink + /proc/self/fd listing + chdir/getcwd"
no  "procselftest: FAIL" "no procself CHECK failures"
no  "KERNEL EXCEPTION|Page fault|General protection|PANIC" "no faults in serial"
if grep -qE "Triple fault" "$INT" 2>/dev/null; then echo "  FAIL: triple fault in int log"; PASS=0; else echo "  OK  : no triple fault"; fi
echo "================================="
if [ "$PASS" = 1 ]; then echo "/proc/self smoke: PASS"; exit 0; else echo "/proc/self smoke: FAIL"; exit 1; fi
