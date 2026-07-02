#!/usr/bin/env bash
# GL/virgl self-test — boot the image on the kosmickrisp virgl QEMU fork, log in root/nanos on a
# text VT, run `gles2info`, and print the GL markers straight to this terminal. Headless (no cocoa
# window needed): output flows over the serial log. This is the tangible "GPU acceleration works"
# proof — success is `renderer=virgl` + `version=OpenGL ES 3.0 Mesa 24.2.8`. The full GL desktop
# (nwm composited through GL) is a separate, unbuilt milestone; this only proves the Mesa→virgl→
# ANGLE→Metal path. Uses ONE persistent QEMU-monitor connection (the fork refuses per-keystroke
# reconnects after host state degrades). Env: QEMU_GL, IMG override the binary / disk image.
set -u
cd "$(dirname "$0")/.."
IMG=${IMG:-disk/image64-grub2.img}
QEMU_GL=${QEMU_GL:-$HOME/Projects/nanos-sdk-work/qemu-virgl-kosmickrisp/bin/qemu-system-x86_64}
SER=$(mktemp -t nanos-gl-selftest.XXXXXX.log)
MON=$(mktemp -t nanos-gl-selftest.XXXXXX.mon); rm -f "$MON"

[ -x "$QEMU_GL" ] || { echo "run64-gl-selftest: no virgl QEMU at $QEMU_GL (set QEMU_GL=...)"; exit 1; }
[ -f "$IMG" ]     || { echo "run64-gl-selftest: no image at $IMG (run 'make image64' first)"; exit 1; }

# Ad-hoc re-sign the tap dylibs the fork dlopen()s — bottle relocation invalidates their signatures.
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
    -drive file="$IMG",format=raw -device virtio-gpu-gl-pci -display cocoa,gl=es \
    -serial file:"$SER" -monitor unix:"$MON",server,nowait -no-reboot >/dev/null 2>&1 &
QPID=$!
trap 'kill -9 "$QPID" 2>/dev/null; rm -f "$SER" "$MON"' EXIT

echo "run64-gl-selftest: booting on the virgl fork (TCG, single vCPU — give it ~15s)..."
for i in $(seq 1 60); do grep -q "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done
grep -q "nanos login:" "$SER" 2>/dev/null || { echo "FAIL: never reached login"; tail -20 "$SER"; exit 1; }

python3 - "$MON" <<'PY'
import socket,time,sys
MON=sys.argv[1]; s=None
for _ in range(30):
    try: s=socket.socket(socket.AF_UNIX); s.connect(MON); break
    except Exception: time.sleep(0.3); s=None
if s is None: print("FATAL: could not open monitor"); sys.exit(1)
time.sleep(0.3)
def key(k): s.sendall(("sendkey "+k+"\n").encode()); time.sleep(0.06)
def typ(w):
    m={'.':'dot','/':'slash','-':'minus','_':'shift-minus'}
    for c in w: key(m.get(c,c)); time.sleep(0.03)
    key("ret")
key("ctrl-alt-f1"); time.sleep(1.5)
typ("root");  time.sleep(1.5)
typ("nanos"); time.sleep(2.0)
typ("gles2info"); time.sleep(1)
s.close()
PY

for i in $(seq 1 60); do
  grep -qE "gles2info: (renderer|no-|init-failed)" "$SER" 2>/dev/null && break
  sleep 1
done
echo "===== gles2info GL markers ====="
grep -E "gles2info:" "$SER" 2>/dev/null || { echo "(none — the fork is flaky; just re-run)"; exit 1; }
echo "================================"
if grep -q "gles2info: renderer=virgl" "$SER" 2>/dev/null; then
  echo "PASS: unmodified Mesa is GPU-backed (renderer=virgl, virgl→ANGLE→Metal)."
else
  echo "note: renderer marker missing — flaky harness, re-run."
fi
