#!/usr/bin/env bash
# gl-desktop-show — open the virgl-fork cocoa gl=es window, auto-log-in to the nwm desktop
# (jan/jan on the graphics VT F7), and LEAVE IT RUNNING so a human can look at the window.
# This is the authoritative display oracle (screendump on gl=es is unreliable). Ctrl-C to quit;
# the QEMU pid is printed so you can `kill` it. Env: QEMU_GL, IMG.
set -u
cd "$(dirname "$0")/.."
IMG=${IMG:-disk/image64.img}
QEMU_GL=${QEMU_GL:-$HOME/Projects/nanos-sdk-work/qemu-virgl-kosmickrisp/bin/qemu-system-x86_64}
SER=$(mktemp -t nanos-glshow.XXXXXX.log)
MON=$(mktemp -t nanos-glshow.XXXXXX.mon); rm -f "$MON"

[ -x "$QEMU_GL" ] || { echo "no virgl QEMU at $QEMU_GL"; exit 1; }
[ -f "$IMG" ]     || { echo "no image at $IMG (run 'make image64')"; exit 1; }

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
    -drive file="$IMG",format=raw -device ${GL_DEV:-virtio-gpu-gl-pci} -vga none \
    -display ${GL_DISPLAY:-cocoa,gl=es} \
    -serial file:"$SER" -monitor unix:"$MON",server,nowait -no-reboot >/dev/null 2>&1 &
QPID=$!
echo "gl-desktop-show: QEMU pid=$QPID  serial=$SER"
echo "gl-desktop-show: booting + auto-login to the nwm desktop (~25s). Watch the cocoa window."
for i in $(seq 1 150); do grep -q "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done
grep -q "nanos login:" "$SER" 2>/dev/null || { echo "never reached login"; tail -15 "$SER"; kill -9 "$QPID"; exit 1; }

MON="$MON" python3 - <<'PY'
import socket,time,os
MON=os.environ["MON"]; s=None
for _ in range(30):
    try: s=socket.socket(socket.AF_UNIX); s.connect(MON); break
    except Exception: time.sleep(0.3); s=None
if s is None: print("no monitor"); raise SystemExit(1)
time.sleep(0.3)
def cmd(c): s.sendall((c+"\n").encode()); time.sleep(0.12)
def key(k): cmd("sendkey "+k); time.sleep(0.06)
def typ(w):
    for c in w: key(c); time.sleep(0.03)
    key("ret")
cmd("sendkey ctrl-alt-f7"); time.sleep(6)
typ("jan"); time.sleep(4)
typ("jan"); time.sleep(2)
print("logged in — nwm should be compositing now")
s.close()
PY

echo "gl-desktop-show: desktop should be up. LOOK AT THE COCOA WINDOW."
echo "gl-desktop-show: QEMU still running as pid=$QPID — kill it with:  kill -9 $QPID"
wait "$QPID"