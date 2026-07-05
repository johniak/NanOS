#!/usr/bin/env bash
# run-gles2info-gl.sh — boot image64 on the virgl QEMU, log in at the text console, run gles2info,
# and capture its serial markers (the visible VT is mirrored to the serial log). This is the
# Task-8 milestone check: unmodified Mesa (gallium-virgl + EGL + GLES2) running NanOS-side, GPU-backed.
#
# Needs the kosmickrisp virgl QEMU (see scripts/run64-gl.sh). GL requires a cocoa window on macOS.
set -u
IMG=disk/image64.img
SER=${SER:-/tmp/nanos-gles2info.log}
MON=/tmp/nanos-gles2info-qmon.sock
QEMU_VIRGL_HOME=${QEMU_VIRGL_HOME:-$HOME/Projects/nanos-sdk-work/qemu-virgl-kosmickrisp}
QEMU_GL=${QEMU_GL:-$QEMU_VIRGL_HOME/bin/qemu-system-x86_64}
[ -x "$QEMU_GL" ] || { echo "no virgl qemu at $QEMU_GL (set QEMU_GL)"; exit 2; }
[ -f "$IMG" ] || { echo "run 'make image64' first"; exit 2; }
rm -f "$SER" "$MON"

# Self-heal the tap dylib signatures (bottle relocation invalidates them -> SIGKILL at launch).
if command -v codesign >/dev/null 2>&1 && command -v brew >/dev/null 2>&1; then
  for keg in libepoxy angle virglrenderer; do
    d=$(brew --prefix "startergo/$keg/$keg" 2>/dev/null)/lib
    [ -d "$d" ] || continue
    for lib in "$d"/*.dylib; do [ -f "$lib" ] || continue
      codesign -v "$lib" >/dev/null 2>&1 || codesign --force --sign - "$lib" >/dev/null 2>&1; done
  done
fi

pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null; sleep 1
"$QEMU_GL" -accel tcg,thread=multi -cpu qemu64 -smp 1 -m 512 -drive file="$IMG",format=raw \
    -device virtio-vga-gl -display "${DISPLAY_BACKEND:-cocoa,gl=es}" \
    -serial file:"$SER" -monitor unix:"$MON",server,nowait -no-reboot &
QPID=$!
cleanup(){ kill -9 "$QPID" 2>/dev/null; }
trap cleanup EXIT

for i in $(seq 1 90); do grep -q "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done
grep -q "nanos login:" "$SER" 2>/dev/null || { echo "FAIL: never reached login"; tail -25 "$SER"; exit 1; }

# Log in root/nanos at the text VT, then run gles2info. Keystrokes go via the PS/2 keyboard to the
# visible VT; that VT's output is mirrored to the serial log we scrape.
python3 - "$MON" <<'PY'
import socket,time,sys
MON=sys.argv[1]
def key(k):
    s=socket.socket(socket.AF_UNIX);
    try: s.connect(MON)
    except Exception as e: print("mon connect failed:",e); return
    time.sleep(0.05)
    s.sendall(("sendkey "+k+"\n").encode()); time.sleep(0.05); s.close()
def typ(word):
    m={'.':'dot','/':'slash','-':'minus','_':'shift-minus'}
    for c in word:
        key(m.get(c, c))
        time.sleep(0.03)
    key("ret")
time.sleep(1)
typ("root"); time.sleep(1)      # login
typ("nanos"); time.sleep(2)     # password
typ("gles2info");
time.sleep(1)
PY

# Wait for the oracle's markers (or a clean failure) on the serial-mirrored console.
for i in $(seq 1 60); do
  grep -qE "gles2info: (clear-readback|renderer=|no-display|no-config|no-context|fbo-incomplete)" "$SER" 2>/dev/null && break
  sleep 1
done

echo "===== gles2info markers ====="
grep -E "gles2info:" "$SER" 2>/dev/null || echo "(no gles2info markers)"
echo "===== faults? ====="
grep -iE "CPU EXCEPTION|KERNEL EXCEPTION|Triple fault|killed faulting|PANIC|#PF|#UD" "$SER" 2>/dev/null | tail -10 || echo "(none)"

if grep -q "gles2info: renderer=virgl" "$SER" 2>/dev/null && grep -q "gles2info: clear-readback OK" "$SER" 2>/dev/null; then
  echo "PASS: Mesa gallium-virgl runs on NanOS (GL_RENDERER=virgl, clear-readback OK)"; exit 0
fi
echo "INCOMPLETE — see $SER (tail below)"; tail -30 "$SER"; exit 1
