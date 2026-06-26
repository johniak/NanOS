#!/usr/bin/env bash
# smoke-usb-smp.sh — the live-USB-on-multicore gate.
#
# Boots the image as a USB mass-storage stick (root-on-USB, like the real Dell) AND with -smp 2,
# the combination that exposed the xHCI event-ring data race: the USB-MSC read path and the USB-HID
# poll thread consumed the same event ring concurrently, so file reads returned corrupted bytes and
# the tty7 greeter exec ran toybox's image ("toybox: Unknown command nwlogin"). Single-CPU smoke-usb
# never reproduced it. This gate switches to the graphics VT (F7), logs in at the greeter, and
# requires the nwm DESKTOP to render — a colour-rich frame only a clean read path can produce.
#
# Pass iff F7 shows the desktop (many distinct colours) and no kernel fault/panic is logged.
set -u
IMG=disk/image64-grub2.img
STICK=/tmp/nanos-usbsmp-stick.img
SER=/tmp/nanos-usbsmp.log
MON=/tmp/nanos-usbsmp-qmon.sock
D=/tmp/nanos-usbsmp-f7.ppm
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }
cp "$IMG" "$STICK"; rm -f "$SER" "$MON" "$D"
pkill -9 -f "qemu-system-x86_64.*$STICK" 2>/dev/null; sleep 1

qemu-system-x86_64 -accel tcg,thread=multi -cpu qemu64 -smp 2 -m 512 \
    -drive if=none,id=usbstick,file="$STICK",format=raw \
    -device qemu-xhci -device usb-storage,drive=usbstick \
    -device usb-kbd -device usb-mouse \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -display none -serial file:"$SER" -monitor unix:"$MON",server,nowait -no-reboot &
QPID=$!
cleanup() { kill -9 "$QPID" 2>/dev/null; }
trap cleanup EXIT

for i in $(seq 1 70); do grep -aq "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done
if ! grep -aq "nanos login:" "$SER" 2>/dev/null; then
	echo "live-USB+SMP: FAIL — never reached login (root-on-USB read path broken under SMP?)"; tail -20 "$SER"; exit 1
fi
sleep 3

python3 - "$MON" "$D" <<'PY'
import socket,time,sys
MON,D=sys.argv[1],sys.argv[2]
def cmd(l):
    s=socket.socket(socket.AF_UNIX)
    try: s.connect(MON)
    except Exception as e: print("monitor connect failed:",e); return
    time.sleep(0.2)
    try: s.settimeout(0.3); s.recv(65536)
    except: pass
    s.sendall((l+"\n").encode()); time.sleep(0.35); s.close()
# Switch to the graphics VT and log in jan/jan at the greeter; generous settles for slow MTTCG.
cmd("sendkey ctrl-alt-f7"); time.sleep(7)
for c in "jan": cmd("sendkey "+c)
cmd("sendkey ret"); time.sleep(4)
for c in "jan": cmd("sendkey "+c)
cmd("sendkey ret"); time.sleep(16)
cmd("screendump "+D); time.sleep(2)
PY

if grep -aqE "Kernel panic|PANIC|TRIPLE FAULT|triple fault|KERNEL EXCEPTION|KERNEL FAULT|exec: short read" "$SER"; then
	echo "live-USB+SMP: FAIL — kernel fault / short read in serial log"
	grep -aE "Kernel panic|PANIC|TRIPLE FAULT|triple fault|KERNEL EXCEPTION|KERNEL FAULT|exec: short read" "$SER" | head
	exit 1
fi
[ -s "$D" ] || { echo "live-USB+SMP: FAIL — no F7 screendump (greeter/desktop never came up)"; tail -15 "$SER"; exit 1; }
NCOL=$(python3 - "$D" <<'PY'
import sys
d=open(sys.argv[1],"rb").read()
assert d[:2]==b"P6"
i=2;tok=[]
while len(tok)<3:
    while d[i] in b" \t\n\r": i+=1
    s=i
    while d[i] not in b" \t\n\r": i+=1
    tok.append(int(d[s:i]))
i+=1; px=d[i:]; cols=set()
for p in range(0,len(px)-2,3): cols.add(px[p]<<16|px[p+1]<<8|px[p+2])
print(len(cols))
PY
)
if [ "${NCOL:-0}" -lt 200 ]; then
	echo "live-USB+SMP: FAIL — tty7 desktop did not render under SMP (only ${NCOL:-0} colours; xHCI read race?)"; tail -15 "$SER"; exit 1
fi
echo "live-USB+SMP: PASS (root-on-USB + -smp 2: nwm desktop rendered on tty7, $NCOL colours)"
exit 0
