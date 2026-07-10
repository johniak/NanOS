#!/usr/bin/env bash
# smoke-usb-storm.sh — concurrent USB-root I/O torture at -smp 4 (the Dell failure mix).
#
# The Dell GL-desktop bring-up fails in ways single-purpose smokes never see: nwm-gl reads a
# 1.6 MiB PNG off the USB root WHILE loggers synchronously append and getty/greeter churn — on
# 4 cores. Symptoms there: intermittent SIGSEGV in the reading process, vanished log appends,
# and a whole-box wedge with USB-backed logs silent. This gate reproduces the MIX in QEMU:
#
#   1. boot the image as root-on-USB (qemu-xhci + usb-storage) with -smp 4 (MTTCG);
#   2. log in on tty1 and run `usbstorm` (parallel CRC-verified readers + synchronous appenders
#      + a shared-file append pair + a deliberate crasher — see user/usbstorm.c);
#   3. SIMULTANEOUSLY log in at the tty7 greeter so nwm starts and loads the wallpaper under storm.
#
# PASS iff serial shows "USBSTORM DONE mism=0 lost=0" (no torn reads, no lost appends, no wedge —
# a wedge means the line never prints), the crasher was killed surgically
# ("[nanos: killed faulting process"), and no kernel fault/panic appears.
set -u
IMG=disk/image64.img
STICK=/tmp/nanos-usbstorm-stick.img
SER=/tmp/nanos-usbstorm.log
MON=/tmp/nanos-usbstorm-qmon.sock
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }
cp "$IMG" "$STICK"; rm -f "$SER" "$MON"
pkill -9 -f "qemu-system-x86_64.*$STICK" 2>/dev/null; sleep 1

qemu-system-x86_64 -accel tcg,thread=multi -cpu qemu64 -smp 4 -m 512 \
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
	echo "usb-storm: FAIL — never reached login"; tail -20 "$SER"; exit 1
fi
sleep 3

python3 - "$MON" <<'PY'
import socket,time,sys
MON=sys.argv[1]
def cmd(l):
    s=socket.socket(socket.AF_UNIX)
    try: s.connect(MON)
    except Exception as e: print("monitor connect failed:",e); return
    time.sleep(0.2)
    try: s.settimeout(0.3); s.recv(65536)
    except: pass
    s.sendall((l+"\n").encode()); time.sleep(0.3); s.close()
def type_line(t):
    for c in t:
        cmd("sendkey "+c)
    cmd("sendkey ret")
# tty1: log in as jan/jan and start the storm in the background.
type_line("jan"); time.sleep(3)
type_line("jan"); time.sleep(5)
type_line("usbstorm")
# tty7 greeter: log in so nwm starts and loads the wallpaper UNDER the storm.
time.sleep(2)
cmd("sendkey ctrl-alt-f7"); time.sleep(7)
type_line("jan"); time.sleep(4)
type_line("jan"); time.sleep(5)
PY

# The storm does ~30 full 1.6 MiB per-sector USB reads + hundreds of synchronous appends under
# MTTCG — give it a generous deadline; a wedge fails by timeout.
DONE=""
for i in $(seq 1 240); do
	if grep -aq "USBSTORM DONE" "$SER" 2>/dev/null; then DONE=1; break; fi
	sleep 2
done

echo "usb-storm: serial assertions"
fail=0
if [ -z "$DONE" ]; then
	echo "  FAIL: storm never finished (wedge?) — last serial lines:"; tail -15 "$SER"; fail=1
else
	grep -a "USBSTORM DONE" "$SER" | tail -1 | sed 's/^/  /'
fi
if grep -aq "USBSTORM MISMATCH" "$SER"; then
	echo "  FAIL: torn/corrupt concurrent read"; grep -a "USBSTORM MISMATCH" "$SER" | head -5; fail=1
fi
if grep -aq "USBSTORM LOST" "$SER"; then
	echo "  FAIL: lost concurrent append"; grep -a "USBSTORM LOST" "$SER" | head -5; fail=1
fi
if ! grep -aq "USBSTORM DONE mism=0 lost=0" "$SER" && [ -n "$DONE" ]; then
	echo "  FAIL: storm finished with failures"; fail=1
fi
if ! grep -aq "killed faulting process" "$SER"; then
	echo "  FAIL: crasher's kill line missing (evidence funnel broken)"; fail=1
else
	echo "  ok: crasher killed surgically ($(grep -ac 'killed faulting process' "$SER") kill line[s])"
fi
if grep -aqE "Kernel panic|PANIC|TRIPLE FAULT|KERNEL EXCEPTION" "$SER"; then
	echo "  FAIL: kernel fault during storm"; grep -aE "Kernel panic|PANIC|TRIPLE FAULT|KERNEL EXCEPTION" "$SER" | head; fail=1
fi
if [ "$fail" = 0 ]; then echo "PASS: concurrent USB-root I/O (4 CPUs): no torn reads, no lost appends, no wedge, surgical kill."; exit 0; fi
echo "FAIL: usb-storm"; exit 1
