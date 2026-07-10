#!/usr/bin/env bash
# smoke-evidence.sh — the crash-evidence-channel gate (Dell GL bring-up).
#
# The Dell's nwm decoder crashes left ZERO [ring3 fault] lines in i915-boot.txt: the panic sink's
# knx_file_append ran with the CRASHED (uid-1000) process current and got -EACCES on the root-owned
# log — the whole post-bind kernel evidence channel silently vanished exactly when it was needed.
# Fixed with the per-CPU kernel-cred scope (kernel/SyscallDispatch.cpp) + the ktimers-thread tee
# flush + the i915 pulse flight recorder. This gate re-runs the original repro so the channel can
# never silently regress:
#
#   1. boot root-on-USB at -smp 4 with the i915 kext ARMED mode '2' (on QEMU the probe aborts at
#      "no Intel GPU", but the log tee + panic sink + pulse thread are registered/started first);
#   2. log in on tty1, run `usbstorm` (its deliberate crasher SIGSEGVs mid-storm);
#   3. read /nanos/logs/i915-boot.txt and /nanos/logs/pulse.txt back off the stick image:
#      PASS iff the [ring3 fault ... comm=usbstorm] line persisted AND the pulse recorded lines.
set -u
IMG=disk/image64.img
STICK=/tmp/nanos-evidence-stick.img
SER=/tmp/nanos-evidence.log
MON=/tmp/nanos-evidence-qmon.sock
KNOB=bin/i915-arm-knob-ev
PART=69206016
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }
cp "$IMG" "$STICK"; rm -f "$SER" "$MON"

printf '2' > "$KNOB"
docker run --rm -v "$(dirname "$STICK")":/stick -v "$PWD":/src -w /src nanos-build bash -c \
  "printf 'rm /nanos/config/i915\nwrite $KNOB /nanos/config/i915\n' | debugfs -w '/stick/$(basename "$STICK")?offset=$PART' >/dev/null 2>&1" \
  || { echo "FAIL: could not arm the i915 knob"; exit 1; }

pkill -9 -f "qemu-system-x86_64.*$STICK" 2>/dev/null; sleep 1
qemu-system-x86_64 -accel tcg,thread=multi -cpu qemu64 -smp 4 -m 512 \
    -drive if=none,id=usbstick,file="$STICK",format=raw \
    -device qemu-xhci -device usb-storage,drive=usbstick \
    -device usb-kbd -device usb-mouse \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -display none -serial file:"$SER" -monitor unix:"$MON",server,nowait -no-reboot &
QPID=$!
cleanup() { kill -9 "$QPID" 2>/dev/null; rm -f "$KNOB" "$STICK"; }
trap cleanup EXIT

for i in $(seq 1 70); do grep -aq "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done
grep -aq "nanos login:" "$SER" 2>/dev/null || { echo "FAIL: never reached login"; tail -20 "$SER"; exit 1; }
grep -aq "armed: DESKTOP mode" "$SER" || { echo "FAIL: i915 did not arm"; grep -ai i915 "$SER" | head; exit 1; }
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
type_line("jan"); time.sleep(3)
type_line("jan"); time.sleep(5)
type_line("usbstorm")
PY

DONE=""
for i in $(seq 1 240); do
	if grep -aq "USBSTORM DONE" "$SER" 2>/dev/null; then DONE=1; break; fi
	sleep 2
done
[ -n "$DONE" ] || { echo "FAIL: storm never finished (wedge?)"; tail -15 "$SER"; exit 1; }
grep -aq "killed faulting process" "$SER" || { echo "FAIL: crasher never faulted"; exit 1; }
if grep -aq "panic-sink append FAILED" "$SER"; then
	echo "FAIL: the panic sink's file append failed (evidence channel broken again)"; exit 1
fi
sleep 3
kill -9 "$QPID" 2>/dev/null; QPID=""; sleep 1

BOOTLOG=/tmp/nanos-evidence-bootlog.txt
PULSE=/tmp/nanos-evidence-pulse.txt
docker run --rm -v "$(dirname "$STICK")":/stick -w /stick nanos-build bash -c \
  "debugfs -R 'cat /nanos/logs/i915-boot.txt' '/stick/$(basename "$STICK")?offset=$PART' 2>/dev/null" > "$BOOTLOG"
docker run --rm -v "$(dirname "$STICK")":/stick -w /stick nanos-build bash -c \
  "debugfs -R 'cat /nanos/logs/pulse.txt' '/stick/$(basename "$STICK")?offset=$PART' 2>/dev/null" > "$PULSE"

fail=0
if grep -q "ring3 fault" "$BOOTLOG" && grep -q "comm=usbstorm" "$BOOTLOG"; then
	echo "  ok: [ring3 fault ... comm=usbstorm] persisted to i915-boot.txt"
else
	echo "  FAIL: [ring3 fault] line missing from i915-boot.txt"; tail -8 "$BOOTLOG"; fail=1
fi
NPULSE=$(grep -c "^pulse t=" "$PULSE" 2>/dev/null || true)
if [ "${NPULSE:-0}" -ge 3 ]; then
	echo "  ok: pulse flight recorder alive ($NPULSE lines)"
else
	echo "  FAIL: pulse recorded ${NPULSE:-0} lines (thread dead or append broken)"; fail=1
fi
if grep -aqE "Kernel panic|PANIC|TRIPLE FAULT|KERNEL EXCEPTION" "$SER"; then
	echo "  FAIL: kernel fault during the run"; fail=1
fi
[ "$fail" = 0 ] && { echo "PASS: ring3-fault evidence + pulse both persist across a crash under USB-root storm."; exit 0; }
echo "FAIL: smoke-evidence"; exit 1
