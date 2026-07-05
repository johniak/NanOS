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
#   D = tty7 after Ctrl+Alt+F7 + logging in at the graphical greeter (login -> nwm desktop)
#
# The D step is the graphics-VT gate: the greeter (login) is itself a TEXT login on tty7, so we
# actually log in (jan/jan) and require the nwm DESKTOP to render — a wallpaper-rich frame with many
# distinct colours. This catches a broken greeter binary (e.g. a stale image where login.nxe is
# really toybox: "Unknown command login"), which can't be logged into and never reaches a desktop.
#
# Pass iff A!=B and A==C, the tty7 desktop renders (D is graphical), and no kernel fault/panic is logged.
set -u
IMG=disk/image64.img
SER=/tmp/nanos-vtsmoke.log
MON=/tmp/nanos-vtsmoke-qmon.sock
A=/tmp/nanos-vt-A.ppm; B=/tmp/nanos-vt-B.ppm; C=/tmp/nanos-vt-C.ppm; D=/tmp/nanos-vt-D.ppm
rm -f "$SER" "$MON" "$A" "$B" "$C" "$D"
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

python3 - "$MON" "$A" "$B" "$C" "$D" <<'PY'
import socket,time,sys
MON,A,B,C,D = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
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
# tty7: the graphical greeter. login is a text login ON the graphics VT, so log in jan/jan and
# require the nwm desktop to come up (verified by the colour-richness oracle below).
keys(["ctrl-alt-f7"]); time.sleep(4.0)         # greeter renders its "NanOS graphical login" prompt
for c in "jan": keys([c])
keys(["ret"]); time.sleep(2.0)                 # username -> password prompt
for c in "jan": keys([c])
keys(["ret"]); time.sleep(10.0)                # auth + setuid + exec nwm + first full desktop paint
cmd("screendump "+D); time.sleep(0.8)          # tty7 — must be the nwm desktop (wallpaper, many colours)
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
# tty7 graphics gate: D must be the nwm desktop, not a text screen (login prompt OR a broken-greeter
# error). A text console is ~black background + white glyphs (a handful of distinct colours); the nwm
# desktop cover-fits a PNG wallpaper (thousands). Count distinct RGB triples in the P6 screendump.
[ -s "$D" ] || { echo "x86_64 VT switch: FAIL — missing tty7 screendump $D (F7/greeter never completed)"; tail -15 "$SER"; exit 1; }
NCOL=$(python3 - "$D" <<'PY'
import sys
with open(sys.argv[1],"rb") as f: data=f.read()
# Parse the P6 header: "P6\n<w> <h>\n<maxval>\n" (tokens may be split across whitespace/newlines).
assert data[:2]==b"P6", "not a P6 PPM"
i=2; tok=[]
while len(tok)<3:
    while i<len(data) and data[i] in b" \t\n\r": i+=1
    s=i
    while i<len(data) and data[i] not in b" \t\n\r": i+=1
    tok.append(int(data[s:i]))
i+=1  # single whitespace after maxval
px=data[i:]
cols=set()
for p in range(0, len(px)-2, 3):           # every pixel; set dedups
    cols.add(px[p]<<16 | px[p+1]<<8 | px[p+2])
print(len(cols))
PY
)
if [ "${NCOL:-0}" -lt 200 ]; then
	echo "x86_64 VT switch: FAIL — tty7 desktop did not render (only ${NCOL:-0} distinct colours; greeter likely broken, e.g. a stale login.nxe)"; tail -15 "$SER"; exit 1
fi
echo "x86_64 VT switch: PASS (tty1 != tty2, tty1 restored, and tty7 nwm desktop rendered: $NCOL colours)"
exit 0
