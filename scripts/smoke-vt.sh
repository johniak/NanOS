#!/usr/bin/env bash
# smoke-vt.sh — the virtual-terminal switching gate (Ctrl+Alt+Fn).
#
# Boots the x86_64 image headless, logs in on tty1, then drives the QEMU monitor's `sendkey
# ctrl-alt-f<N>` to switch consoles and screendumps the framebuffer at each step. The oracle is
# byte-exact and deterministic (no OCR): switching to a DIFFERENT console must change the screen,
# and switching BACK must restore it exactly — proving the kernel VT subsystem (per-VT fbcon grids,
# input routing, repaint-on-switch) works end to end.
#
#   A = tty1 after login (bash prompt)
#   B = tty2 after Ctrl+Alt+F2 (a distinct, independent login)   -> A != B
#   C = tty1 after Ctrl+Alt+F1 (back)                            -> A == C  (screen restored)
#
# Pass iff A!=B and A==C and no kernel fault/panic is logged.
set -u
IMG=disk/image64-grub2.img
SER=/tmp/nanos-vtsmoke.log
MON=/tmp/nanos-vtsmoke-qmon.sock
A=/tmp/nanos-vt-A.ppm; B=/tmp/nanos-vt-B.ppm; C=/tmp/nanos-vt-C.ppm
rm -f "$SER" "$MON" "$A" "$B" "$C"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }

pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null
sleep 1
qemu-system-x86_64 -accel tcg,thread=multi -cpu qemu64 -smp 2 -m 512 -drive file="$IMG",format=raw \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -display none -serial file:"$SER" -monitor unix:"$MON",server,nowait -no-reboot &
QPID=$!
cleanup() { kill -9 "$QPID" 2>/dev/null; }
trap cleanup EXIT

for i in $(seq 1 60); do grep -q "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done
if ! grep -q "nanos login:" "$SER" 2>/dev/null; then
	echo "FAIL: never reached login"; tail -20 "$SER" 2>/dev/null; exit 1
fi

python3 - "$MON" "$A" "$B" "$C" <<'PY'
import socket,time,sys
MON,A,B,C = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
def cmd(line):
    s=socket.socket(socket.AF_UNIX)
    try: s.connect(MON)
    except Exception as e: print("monitor connect failed:",e); return
    time.sleep(0.15)
    try: s.settimeout(0.3); s.recv(65536)
    except: pass
    s.sendall((line+"\n").encode()); time.sleep(0.2); s.close()
def keys(ks):
    for k in ks: cmd("sendkey "+k)
# Log in on tty1 as jan/jan (the seeded account); generous settles so the shell is fully up.
for c in "jan": keys([c])
keys(["ret"]); time.sleep(3.0)
for c in "jan": keys([c])
keys(["ret"]); time.sleep(7.0)                 # let tty1's bash prompt fully render + go idle (no blink)
cmd("screendump "+A); time.sleep(0.8)          # tty1 (bash) — must be a STABLE frame for the A==C oracle
keys(["ctrl-alt-f2"]); time.sleep(3.5)
cmd("screendump "+B); time.sleep(0.8)          # tty2 (fresh login)
keys(["ctrl-alt-f1"]); time.sleep(3.5)         # switch back; the kernel repaints tty1's saved grid
cmd("screendump "+C); time.sleep(0.8)          # back to tty1 — must equal A (state restored)
PY
sleep 1

if grep -aqE "Kernel panic|PANIC|TRIPLE FAULT|triple fault|KERNEL EXCEPTION|KERNEL FAULT" "$SER"; then
	echo "x86_64 VT switch: FAIL — kernel fault/panic in serial log"
	grep -aE "Kernel panic|PANIC|TRIPLE FAULT|triple fault|KERNEL EXCEPTION|KERNEL FAULT" "$SER" | head
	exit 1
fi
for f in "$A" "$B" "$C"; do
	[ -s "$f" ] || { echo "x86_64 VT switch: FAIL — missing screendump $f (switch/login never completed)"; tail -15 "$SER"; exit 1; }
done
if cmp -s "$A" "$B"; then
	echo "x86_64 VT switch: FAIL — Ctrl+Alt+F2 did not change the screen (tty1 == tty2)"; exit 1
fi
if ! cmp -s "$A" "$C"; then
	echo "x86_64 VT switch: FAIL — Ctrl+Alt+F1 did not restore tty1 (tty1 != tty1-after-roundtrip)"; exit 1
fi
echo "x86_64 VT switch: PASS (tty1 != tty2, and tty1 restored after the round-trip)"
exit 0
