#!/usr/bin/env bash
# gl-desktop-test — the A/B other half of smoke-virtio-gpu.sh. SAME image, SAME desktop bring-up
# (log in jan/jan on the graphics VT F7, let nwm render), but on the kosmickrisp virgl QEMU fork
# with the cocoa gl=es display — the exact `make run64-gl` device/display combo. Screendumps the
# desktop and counts distinct colours, identically to smoke-virtio-gpu.sh, so the two are directly
# comparable:
#   stock virtio-gpu (smoke-virtio-gpu.sh): 4322 distinct colours = desktop renders.
#   this (gl fork):  rich  → the desktop displays on gl=es too (2D scanout reaches the GL surface).
#                    black → either the screendump can't read gl=es, or the fork doesn't present the
#                            2D scanout — look at the actual cocoa window to disambiguate.
# The cocoa window is left open on screen so a human can eyeball it (the authoritative oracle).
set -u
cd "$(dirname "$0")/.."
IMG=${IMG:-disk/image64-grub2.img}
QEMU_GL=${QEMU_GL:-$HOME/Projects/nanos-sdk-work/qemu-virgl-kosmickrisp/bin/qemu-system-x86_64}
SER=$(mktemp -t nanos-gldesk.XXXXXX.log)
MON=$(mktemp -t nanos-gldesk.XXXXXX.mon); rm -f "$MON"
DESK=$(mktemp -t nanos-gldesk.XXXXXX.ppm)

[ -x "$QEMU_GL" ] || { echo "gl-desktop-test: no virgl QEMU at $QEMU_GL"; exit 1; }
[ -f "$IMG" ]     || { echo "gl-desktop-test: no image at $IMG (run 'make image64')"; exit 1; }

if command -v codesign >/dev/null 2>&1 && command -v brew >/dev/null 2>&1; then
  for keg in libepoxy angle virglrenderer; do
    d=$(brew --prefix "startergo/$keg/$keg" 2>/dev/null)/lib
    [ -d "$d" ] || continue
    for lib in "$d"/*.dylib; do [ -f "$lib" ] || continue
      codesign -v "$lib" >/dev/null 2>&1 || codesign --force --sign - "$lib" >/dev/null 2>&1; done
  done
fi

pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null; sleep 1
"$QEMU_GL" -cpu qemu64 -accel tcg,thread=multi -smp 1 -m 512 \
    -drive file="$IMG",format=raw -device ${GL_DEV:-virtio-gpu-gl-pci} -display ${GL_DISPLAY:-cocoa,gl=es} \
    -serial file:"$SER" -monitor unix:"$MON",server,nowait -no-reboot >/dev/null 2>&1 &
QPID=$!
trap 'kill -9 "$QPID" 2>/dev/null; rm -f "$MON"' EXIT
echo "gl-desktop-test: serial=$SER  desk-ppm=$DESK"
echo "gl-desktop-test: booting the virgl fork + cocoa gl=es window (~20s)..."
for i in $(seq 1 150); do grep -q "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done
grep -q "nanos login:" "$SER" 2>/dev/null || { echo "FAIL: never reached login"; tail -20 "$SER"; exit 1; }

MON="$MON" DESK="$DESK" python3 - <<'PY'
import socket,time,os
MON=os.environ["MON"]; s=None
for _ in range(30):
    try: s=socket.socket(socket.AF_UNIX); s.connect(MON); break
    except Exception: time.sleep(0.3); s=None
if s is None: print("FATAL: no monitor"); raise SystemExit(1)
time.sleep(0.3)
def cmd(c): s.sendall((c+"\n").encode()); time.sleep(0.12)
def key(k): cmd("sendkey "+k); time.sleep(0.06)
def typ(w):
    for c in w: key(c); time.sleep(0.03)
    key("ret")
cmd("sendkey ctrl-alt-f7"); time.sleep(6)   # graphics VT (greeter)
typ("jan"); time.sleep(4)                    # user
typ("jan"); time.sleep(20)                   # password -> nwm desktop composites
cmd("screendump "+os.environ["DESK"]); time.sleep(3)
s.close()
PY

echo "===== serial (virtio_gpu / present / faults) ====="
grep -iE "virtio_gpu|scanout|present|EXCEPTION|fault|PANIC" "$SER" 2>/dev/null | tail -20
echo "=================================================="
[ -s "$DESK" ] || { echo "FAIL: no screendump"; exit 1; }
COLS=$(python3 - "$DESK" <<'PY'
import sys
d=open(sys.argv[1],'rb').read()
i=0
for _ in range(3): i=d.index(b'\n',i)+1
px=d[i:]; cols=set()
for k in range(0,len(px)-3,3*97): cols.add(px[k:k+3])
print(len(cols))
PY
)
echo "gl-fork desktop distinct colours sampled: $COLS   (stock virtio-gpu = 4322)"
if [ "${COLS:-0}" -ge 200 ]; then
  echo "RESULT: desktop DISPLAYS on the gl=es scanout (screendump rich)."
else
  echo "RESULT: gl=es screendump is blank ($COLS colours) — look at the cocoa window to tell"
  echo "        'screendump-can't-read-gl=es' (window shows desktop) from 'fork-doesn't-present"
  echo "        2D scanout' (window also black)."
fi
echo "PPM kept: $DESK"