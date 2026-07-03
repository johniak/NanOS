#!/usr/bin/env bash
# smoke-virtio-gpu-gl.sh — the Task-10 GL desktop gate: nwm's GL ES present backend (nw_compose_gl.c)
# composites the desktop and scans it out through GBM+EGL+KMS (glkms) instead of blitting /dev/fb0.
#
# This gate needs BOTH a virgl-capable QEMU (the startergo/qemu-virgl-kosmickrisp fork) AND a macOS
# windowed GUI session (cocoa gl=es — the host GL backend is ANGLE→Metal, which has no headless
# path, and the gl=es scanout cannot be read by the QEMU monitor `screendump`). It is therefore a
# DEVELOPER gate (run it on your Mac), NOT part of headless `verify64`. It SKIPs cleanly (exit 0)
# when the fork QEMU is absent so it never breaks an automated run.
#
# Oracle:
#   serial (hard):  "virgl 3D negotiated", "glkms: mode WxH", "nwm: GL compositor active",
#                   and NO "GL backend disabled" / EXCEPTION / PANIC / BUG:.
#   window (extra): a screencapture of the cocoa window has many distinct colours (the glass
#                   desktop), asserted >= 200 when the GUI capture is available.
set -u
cd "$(dirname "$0")/.."
IMG=disk/image64-gl-grub2.img
QEMU_VIRGL_HOME=${QEMU_VIRGL_HOME:-$HOME/Projects/nanos-sdk-work/qemu-virgl-kosmickrisp}
QEMU_GL=${QEMU_GL:-$QEMU_VIRGL_HOME/bin/qemu-system-x86_64}
SER=/tmp/nanos-glgate.log
MON=/tmp/nanos-glgate.mon
SHOT=/tmp/nanos-glgate-window.png
rm -f "$SER" "$MON" "$SHOT"

[ -x "$QEMU_GL" ] || { echo "SKIP: no virgl-capable QEMU at $QEMU_GL (set QEMU_GL)"; exit 0; }
[ -f "$IMG" ]     || { echo "SKIP: no $IMG — run 'make image64-gl' first"; exit 0; }

# self-heal tap dylib signatures (Apple Silicon bottle-relocation invalidates them)
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
    -drive file="$IMG",format=raw -device virtio-gpu-gl-pci -vga none \
    -display cocoa,gl=es,zoom-to-fit=on \
    -serial file:"$SER" -monitor unix:"$MON",server,nowait -no-reboot >/dev/null 2>&1 &
QPID=$!
trap 'kill -9 "$QPID" 2>/dev/null; rm -f "$MON"' EXIT
echo "smoke-virtio-gpu-gl: QEMU pid=$QPID serial=$SER"

for i in $(seq 1 180); do grep -q "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done
grep -q "nanos login:" "$SER" 2>/dev/null || { echo "FAIL: never reached login"; tail -25 "$SER"; exit 1; }

# drive the graphical VT login (jan/jan on F7), same as smoke-vt / smoke-virtio-gpu
MON="$MON" python3 - <<'PY'
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
cmd("sendkey ctrl-alt-f7"); time.sleep(6)
typ("jan"); time.sleep(4)
typ("jan"); time.sleep(2)
s.close()
PY
sleep 12   # let the desktop compose + present a few GL frames

fail=0
assert_ser() { grep -q "$1" "$SER" 2>/dev/null && echo "  ok: $1" || { echo "  MISSING: $1"; fail=1; }; }
echo "smoke-virtio-gpu-gl: serial assertions"
assert_ser "virgl 3D negotiated"
assert_ser "glkms: mode "
assert_ser "nwm: GL compositor active"
if grep -qE "GL backend disabled|EXCEPTION|PANIC|BUG:" "$SER" 2>/dev/null; then
  echo "  FAIL: found a disable/fault marker:"; grep -nE "GL backend disabled|EXCEPTION|PANIC|BUG:" "$SER" | tail -8; fail=1
fi
[ "$fail" = 0 ] || { echo "FAIL: GL desktop serial oracle not satisfied"; exit 1; }

# extra visual oracle: screencapture the cocoa window and count distinct colours (best-effort).
RECT=$(osascript -e 'tell application "System Events" to tell (first process whose name contains "qemu-system") to get {position, size} of front window' 2>/dev/null | tr -d ' ')
if [ -n "$RECT" ] && command -v screencapture >/dev/null 2>&1; then
  X=$(echo "$RECT"|cut -d, -f1); Y=$(echo "$RECT"|cut -d, -f2); W=$(echo "$RECT"|cut -d, -f3); H=$(echo "$RECT"|cut -d, -f4)
  screencapture -x -R"${X},${Y},${W},${H}" "$SHOT" 2>/dev/null
  COLS=$(python3 - "$SHOT" <<'PY'
import sys
try:
    from PIL import Image
    im=Image.open(sys.argv[1]).convert("RGB"); c=im.getcolors(maxcolors=1000000)
    print(len(c) if c else 1000000)
except Exception: print(-1)
PY
)
  if [ "${COLS:-0}" -ge 200 ]; then echo "  ok: window has $COLS distinct colours (glass desktop)"
  elif [ "${COLS:-0}" = "-1" ]; then echo "  note: PIL unavailable, skipped colour count (serial oracle already PASSED)"
  else echo "FAIL: window only $COLS distinct colours (blank?)"; exit 1; fi
else
  echo "  note: no GUI window capture available; serial oracle already PASSED"
fi

echo "PASS: nwm GL present backend composites + scans out the desktop via GBM+EGL+KMS (glkms)."
exit 0
