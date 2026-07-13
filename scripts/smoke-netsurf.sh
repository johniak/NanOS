#!/usr/bin/env bash
# smoke-netsurf.sh — the NetSurf browser-on-desktop gate.
#
# Boots the x86_64 image headless, logs in at the tty7 graphical greeter (jan/jan -> nwm
# desktop), opens the Run dialog (Super+R), launches `netsurf`, and screendumps the
# framebuffer. NetSurf starts argless on its baked-in welcome.html (file:// — no network
# needed), whose page body is near-white; the desktop underneath is dark glass. The oracle
# is therefore colourimetric and deterministic (no OCR): the near-white pixel fraction must
# JUMP between the desktop-only frame and the NetSurf frame (browser window rendered), and
# the two frames must differ. A missing/broken netsurf.nxe leaves the desktop unchanged.
#
# Pass iff the near-white fraction rises by >= 8 percentage points (and the frames differ)
# and no kernel fault/panic is logged. Requires netsurf installed in the image (make netsurf
# + make image64); fails fast if bin/netsurf.nxe was never staged.
set -u
IMG=${IMG:-disk/image64.img}
SER=/tmp/nanos-nssmoke.log
MON=/tmp/nanos-nssmoke-qmon.sock
DESK=/tmp/nanos-ns-desk.ppm; NS=/tmp/nanos-ns-netsurf.ppm
rm -f "$SER" "$MON" "$DESK" "$NS"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }
[ -f bin/netsurf.nxe ] || { echo "FAIL: bin/netsurf.nxe not staged — run 'make netsurf' (or 'make externals') + 'make image64'"; exit 2; }

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

python3 - "$MON" "$DESK" "$NS" <<'PY'
import socket,time,sys
MON,DESK,NS = sys.argv[1], sys.argv[2], sys.argv[3]
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
def type_word(w):                       # lowercase only: sendkey has no shift map here
    for c in w: keys([c])
# tty7: graphical greeter -> nwm desktop (same choreography as smoke-vt.sh).
keys(["ctrl-alt-f7"]); time.sleep(4.0)
type_word("jan"); keys(["ret"]); time.sleep(2.0)
type_word("jan"); keys(["ret"]); time.sleep(12.0)   # auth + nwm + first full desktop paint
cmd("screendump "+DESK); time.sleep(0.8)
# Super+R Run dialog -> "netsurf" -> Enter. NetSurf loads 8.7MB + res + renders welcome.html;
# TCG is slow, so settle generously before the frame grab.
keys(["meta_l-r"]); time.sleep(2.0)
type_word("netsurf"); keys(["ret"])
time.sleep(45.0)
cmd("screendump "+NS); time.sleep(0.8)
PY
sleep 1

if grep -aqE "Kernel panic|PANIC|TRIPLE FAULT|triple fault|KERNEL EXCEPTION|KERNEL FAULT" "$SER"; then
	echo "netsurf smoke: FAIL — kernel fault/panic in serial log"
	grep -aE "Kernel panic|PANIC|TRIPLE FAULT|triple fault|KERNEL EXCEPTION|KERNEL FAULT" "$SER" | head
	exit 1
fi
for f in "$DESK" "$NS"; do
	[ -s "$f" ] || { echo "netsurf smoke: FAIL — missing screendump $f"; tail -15 "$SER"; exit 1; }
done
if cmp -s "$DESK" "$NS"; then
	echo "netsurf smoke: FAIL — screen unchanged after launching netsurf (app never started?)"; exit 1
fi
read D0 N0 <<EOF2
$(python3 - "$DESK" "$NS" <<'PY'
import sys
def white_frac(path):
    with open(path,"rb") as f: data=f.read()
    assert data[:2]==b"P6", "not a P6 PPM"
    i=2; tok=[]
    while len(tok)<3:
        while i<len(data) and data[i] in b" \t\n\r": i+=1
        s=i
        while i<len(data) and data[i] not in b" \t\n\r": i+=1
        tok.append(int(data[s:i]))
    i+=1
    px=data[i:]
    total=0; white=0
    for p in range(0, len(px)-2, 3):
        total+=1
        if px[p]>200 and px[p+1]>200 and px[p+2]>200: white+=1
    return white/max(total,1)
print(round(white_frac(sys.argv[1])*100,1), round(white_frac(sys.argv[2])*100,1))
PY
)
EOF2
GAIN=$(python3 -c "print(1 if $N0 - $D0 >= 8.0 else 0)")
if [ "$GAIN" != "1" ]; then
	echo "netsurf smoke: FAIL — no browser page on screen (near-white: desktop ${D0}% -> netsurf ${N0}%)"
	exit 1
fi
echo "netsurf smoke: PASS (near-white: desktop ${D0}% -> netsurf ${N0}% — welcome page rendered)"
exit 0
