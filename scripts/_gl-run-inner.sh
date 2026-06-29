#!/usr/bin/env bash
# _gl-run-inner.sh — runs INSIDE the nanos-gltest container (Linux, native arch).
# Boots the x86_64 NanOS image under QEMU with the virtio-gpu 3D (virgl) device,
# software-rendered headless via Xvfb + gtk,gl=on + Mesa llvmpipe (no /dev/dri needed).
#
# Env in:
#   IMG   disk image (default disk/image64-grub2.img, relative to /work)
#   SER   serial log path (default /work/disk/nanos-gl.log)
#   MON   monitor unix socket (default /tmp/nanos-gl-qmon.sock)
#   MODE  "wait"     -> boot, wait for login, then tail serial forever (dev/run)
#         "shot"     -> boot, login on tty7 (jan/jan), screendump to $DESK, exit
#   DESK  screendump output PPM (MODE=shot; default /work/disk/nanos-gl.ppm)
#   BOOT_TIMEOUT  seconds to wait for "nanos login:" (default 180)
# Extra args after -- are passed to qemu.
set -u
IMG=${IMG:-disk/image64-grub2.img}
SER=${SER:-/work/disk/nanos-gl.log}
MON=${MON:-/tmp/nanos-gl-qmon.sock}
MODE=${MODE:-wait}
DESK=${DESK:-/work/disk/nanos-gl.ppm}
BOOT_TIMEOUT=${BOOT_TIMEOUT:-180}
rm -f "$SER" "$MON" "$DESK"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }

# Xvfb gives GLX a software (llvmpipe) GL context with no GPU / no DRM render node.
export DISPLAY=:99
Xvfb :99 -screen 0 1280x900x24 -nolisten tcp >/tmp/xvfb.log 2>&1 &
XPID=$!
sleep 1

qemu-system-x86_64 -accel tcg,thread=multi -cpu qemu64 -smp 1 -m 512 \
    -drive file="$IMG",format=raw \
    -device virtio-vga-gl -display gtk,gl=on,show-menubar=off,zoom-to-fit=off \
    -serial file:"$SER" -monitor unix:"$MON",server,nowait -no-reboot "$@" &
QPID=$!
cleanup() { kill -9 "$QPID" "$XPID" 2>/dev/null; }
trap cleanup EXIT

for i in $(seq 1 "$BOOT_TIMEOUT"); do
    grep -q "nanos login:" "$SER" 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || { echo "FAIL: qemu exited before login"; tail -20 "$SER"; exit 1; }
    sleep 1
done
if ! grep -q "nanos login:" "$SER" 2>/dev/null; then
    echo "FAIL: never reached login in ${BOOT_TIMEOUT}s"; tail -25 "$SER"; exit 1
fi
echo "=== reached 'nanos login:' in ${i}s ==="

if [ "$MODE" = "wait" ]; then
    echo "=== qemu up (virtio-vga-gl, llvmpipe); tailing $SER (Ctrl-C / docker stop to quit) ==="
    exec tail -f "$SER"
fi

# MODE=shot: drive the graphics VT login (keys via the QEMU monitor), then capture the
# frame from the X server — `gtk,gl=on` rasterizes the guest's GL scanout into the Xvfb
# framebuffer, and `screendump` does not work on a GL scanout ("no surface"), so we grab
# the X root instead (xwd -> ppm).
python3 - "$MON" <<'PY'
import socket,time,sys
MON=sys.argv[1]
def cmd(line):
    s=socket.socket(socket.AF_UNIX); s.connect(MON); time.sleep(0.15)
    try: s.settimeout(0.3); s.recv(65536)
    except: pass
    s.sendall((line+"\n").encode()); time.sleep(0.2); s.close()
cmd("sendkey ctrl-alt-f7"); time.sleep(6)
for c in "jan": cmd("sendkey "+c)
cmd("sendkey ret"); time.sleep(4)
for c in "jan": cmd("sendkey "+c)
cmd("sendkey ret"); time.sleep(20)
PY
xwd -root -silent -display :99 2>/dev/null | xwdtopnm 2>/dev/null > "$DESK"
[ -s "$DESK" ] && echo "=== captured X framebuffer -> $DESK ($(wc -c <"$DESK") bytes) ===" \
              || echo "=== CAPTURE FAILED ==="
