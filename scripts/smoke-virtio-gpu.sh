#!/usr/bin/env bash
# smoke-virtio-gpu.sh — the LinuxKPI virtio-gpu display gate.
#
# Boots the x86_64 image headless with the virtio-gpu device as the ONLY display
# (`-vga none -device virtio-gpu-pci`) — there is no VBE/std-VGA linear framebuffer to fall back
# on, so ANY pixels on screen must have been scanned out by the virtio_gpu path. The kext loaded
# is the FULL DRM lift: the UNMODIFIED Linux 6.12 virtio_gpu DRM driver + DRM/KMS core compiled
# against the LinuxKPI shim (kext/virtio_gpu + linuxkpi/). The bring-up (virtio_gpu_present.c)
# drives scanout 0 through the driver's own command layer and bridges it to /dev/fb0.
#
# Oracle (no OCR):
#   1. serial shows the unmodified driver probed:   "virtio_gpu_probe() OK"
#      and the scanout/fb0 bridge came up:           "scanout 0 up via the unmodified DRM driver"
#   2. after switching to the graphics VT (Ctrl+Alt+F7) and logging in jan/jan, the nwm DESKTOP
#      renders — a wallpaper-rich frame with many distinct colours (proves live present through
#      the unmodified driver, not just a cleared scanout).
#   3. no kernel fault/panic on the serial console.
set -u
IMG=disk/image64.img
SER=/tmp/nanos-vgpusmoke.log
MON=/tmp/nanos-vgpusmoke-qmon.sock
DESK=/tmp/nanos-vgpu-desk.ppm
rm -f "$SER" "$MON" "$DESK"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }

pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null
sleep 1
# virtio-gpu is the ONLY display (-vga none); single core keeps the cooperative kext bring-up simple.
qemu-system-x86_64 -accel tcg,thread=multi -cpu qemu64 -smp 1 -m 512 -drive file="$IMG",format=raw \
    -vga none -device virtio-gpu-pci \
    -display none -serial file:"$SER" -monitor unix:"$MON",server,nowait -no-reboot &
QPID=$!
cleanup() { kill -9 "$QPID" 2>/dev/null; }
trap cleanup EXIT

for i in $(seq 1 60); do grep -q "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done
if ! grep -q "nanos login:" "$SER" 2>/dev/null; then
	echo "FAIL: never reached login (boot stalled — virtio-gpu bring-up hang?)"; tail -25 "$SER" 2>/dev/null; exit 1
fi

# 1) the UNMODIFIED driver probed and the scanout/fb0 bridge came up
if ! grep -q "virtio_gpu_probe() OK" "$SER" 2>/dev/null; then
	echo "FAIL: unmodified virtio_gpu_probe() did not complete"; grep -i "virtio_gpu\|drm" "$SER" | tail -20; exit 1
fi
if ! grep -q "scanout 0 up via the unmodified DRM driver" "$SER" 2>/dev/null; then
	echo "FAIL: scanout/fb0 bridge not established"; grep -i "virtio_gpu\|drm" "$SER" | tail -20; exit 1
fi

# 2) switch to the graphics VT, log in jan/jan, screendump the desktop
python3 - "$MON" "$DESK" <<'PY'
import socket,time,sys
MON,DESK = sys.argv[1], sys.argv[2]
def cmd(line):
    s=socket.socket(socket.AF_UNIX)
    try: s.connect(MON)
    except Exception as e: print("monitor connect failed:",e); return
    time.sleep(0.15)
    try: s.settimeout(0.3); s.recv(65536)
    except: pass
    s.sendall((line+"\n").encode()); time.sleep(0.2); s.close()
cmd("sendkey ctrl-alt-f7"); time.sleep(6)
for c in "jan": cmd("sendkey "+c)
cmd("sendkey ret"); time.sleep(4)
for c in "jan": cmd("sendkey "+c)
cmd("sendkey ret"); time.sleep(18)
cmd("screendump "+DESK); time.sleep(3)
PY

# 3) no kernel fault
if grep -qiE "CPU EXCEPTION|KERNEL EXCEPTION|Triple fault|killed faulting process|PANIC" "$SER" 2>/dev/null; then
	echo "FAIL: kernel fault on the console"; grep -iE "EXCEPTION|fault|PANIC" "$SER" | tail; exit 1
fi

# desktop richness: count distinct sampled colours in the PPM (a cleared/blank scanout has ~1)
[ -s "$DESK" ] || { echo "FAIL: no screendump produced"; exit 1; }
COLS=$(python3 - "$DESK" <<'PY'
import sys
d=open(sys.argv[1],'rb').read()
# skip 3 PPM header lines (P6\n W H\n maxval\n)
i=0
for _ in range(3): i=d.index(b'\n',i)+1
px=d[i:]; cols=set()
for k in range(0,len(px)-3,3*97): cols.add(px[k:k+3])
print(len(cols))
PY
)
echo "virtio-gpu desktop distinct colours sampled: $COLS"
if [ "${COLS:-0}" -lt 200 ]; then
	echo "FAIL: virtio-gpu scanout not a desktop (only $COLS distinct colours — blank/cleared?)"; exit 1
fi

echo "PASS: unmodified Linux virtio_gpu DRM driver drives the nwm desktop on /dev/fb0 (-vga none)."
exit 0
