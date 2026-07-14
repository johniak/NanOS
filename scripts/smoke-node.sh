#!/usr/bin/env bash
# smoke-node.sh — Node core acceptance gate (plan 02): `node --jitless -e "console.log(1+2)"` -> 3.
# Proves node.nxe starts V8, executes JavaScript, and prints to stdout on NanOS. Provably-can-fail:
# before node ran JS the command produced no `3`. Startup under TCG is a few seconds (sbrk batching +
# cached malloc-lock tid brought it from ~60-90 s down to ~6 s), so a 45 s window is comfortable.
# NOTE: requires bin/node.nxe staged into the image (make node && make image64); the fs/net/dns/timers
# suite (node-smoke.js) is a separate, later gate.
set -u
IMG=disk/image64.img
SER=/tmp/nanos-node-serial.log
INT=/tmp/nanos-node-int.log
MON=/tmp/nanos-node-qmon.sock
rm -f "$SER" "$INT" "$MON"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }
if ! grep -q "node.nxe" <(ls disk 2>/dev/null) 2>/dev/null; then :; fi   # node lives inside the image fs
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
# QEMU sendkey names for the shifted characters in the command line.
SYM={' ':'spc','-':'minus','.':'dot','"':'shift-apostrophe','(':'shift-9',')':'shift-0','+':'shift-equal'}
def typ(text, settle):
    for c in text:
        s.sendall(("sendkey "+SYM.get(c,c)+"\n").encode()); time.sleep(0.06)
    s.sendall(b"sendkey ret\n"); time.sleep(settle)
typ("jan", 1.5)                                        # username
typ("jan", 2.5)                                        # password -> bash login shell
typ('node --jitless -e "console.log(1+2)"', 45.0)      # V8 isolate init + JS eval + stdout
s.close()
PY
sleep 2
# Strip ANSI escapes (node colorizes TTY output) so the numeric result is greppable on its own line.
STRIP=/tmp/nanos-node-serial.stripped
sed -r 's/\x1b\[[0-9;]*[A-Za-z]//g' "$SER" > "$STRIP" 2>/dev/null || cp "$SER" "$STRIP"
PASS=1
chk(){ if grep -q "$1" "$STRIP" 2>/dev/null; then echo "  OK  : $2"; else echo "  FAIL: $2 (missing: $1)"; PASS=0; fi; }
no(){ if grep -qE "$1" "$STRIP" 2>/dev/null; then echo "  FAIL: $2"; PASS=0; else echo "  OK  : $2"; fi; }
echo "=== Node core JS-eval smoke ==="
chk "jan@nanos"                                          "reached the shell"
chk 'node --jitless -e'                                  "node command launched"
# A line that is exactly '3' (console.log(1+2)) appearing AFTER the command echo.
if awk '/node --jitless -e/{seen=1;next} seen && /^3\r?$/{found=1} END{exit !found}' "$STRIP"; then
  echo "  OK  : console.log(1+2) printed 3"
else
  echo "  FAIL: console.log(1+2) did not print 3"; PASS=0
fi
no  "KERNEL EXCEPTION|Page fault|General protection|PANIC" "no faults in serial"
if grep -qE "Triple fault" "$INT" 2>/dev/null; then echo "  FAIL: triple fault in int log"; PASS=0; else echo "  OK  : no triple fault"; fi
echo "================================="
if [ "$PASS" = 1 ]; then echo "node smoke: PASS"; exit 0; else echo "node smoke: FAIL"; exit 1; fi
