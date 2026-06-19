#!/usr/bin/env bash
# smoke-usb.sh — the live-USB boot smoke: boot with the ENTIRE root filesystem on a USB
# mass-storage device (no -drive disk), and assert the kernel's in-kernel USB storage path
# (xHCI + USB core + MSC) discovered the controller, mounted /disks/main over USB-MSC, reached
# the shell, ran a ring-3 fork/exec, and took ZERO faults. (usb-kbd / usb-mouse + the HID-into-NanWM
# assertions are added once the usbhid kext lands — Task 6; attaching a usb-kbd now would only
# steal the monitor's sendkey input since nothing drives it yet.)
#
# Usage: scripts/smoke-usb.sh              (boots disk/image64-grub2.img as a USB stick; build first)
# Exit 0 = PASS, non-zero = FAIL (offending serial/int lines printed).
set -u
IMG=disk/image64-grub2.img
USBIMG=/tmp/nanos-usbsmoke-stick.img
SER=/tmp/nanos-usbsmoke-serial.log
INT=/tmp/nanos-usbsmoke-int.log
MON=/tmp/nanos-usbsmoke-qmon.sock
rm -f "$SER" "$INT" "$MON"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }
# A private copy as the USB stick backing (the smoke writes to it via the boot self-test).
cp "$IMG" "$USBIMG"

pkill -9 -f "qemu-system-x86_64.*$USBIMG" 2>/dev/null
qemu-system-x86_64 -cpu qemu64 -m 512 \
    -drive if=none,id=usbstick,file="$USBIMG",format=raw \
    -device qemu-xhci -device usb-storage,drive=usbstick \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -display none -serial file:"$SER" -monitor unix:"$MON",server,nowait \
    -no-reboot -d int,cpu_reset -D "$INT" &
QPID=$!
cleanup() { kill -9 "$QPID" 2>/dev/null; rm -f "$USBIMG"; }
trap cleanup EXIT

# 1) wait for the shell prompt (means USB root mounted + scheduler + init->shell all ran)
for i in $(seq 1 45); do grep -q "bash-5\|starting shell" "$SER" 2>/dev/null && break; sleep 1; done

# 2) drive a ring-3 fork/exec from the console (keyboard->tty->fork->exec).
python3 - "$MON" <<'PY'
import socket,time,sys
KM={' ':'spc','_':'shift-minus','\n':'ret'}
def kn(c):
    if c in KM: return KM[c]
    if c.isdigit(): return c
    if c.isalpha(): return ('shift-'+c.lower()) if c.isupper() else c
    return None
s=socket.socket(socket.AF_UNIX)
try: s.connect(sys.argv[1])
except Exception as e: print("monitor connect failed:",e); sys.exit(0)
time.sleep(0.3)
try: s.settimeout(0.3); s.recv(65536)
except: pass
for c in "echo USB_SMOKE_FORK_OK":
    k=kn(c)
    if k: s.sendall(("sendkey "+k+"\n").encode()); time.sleep(0.04)
    try: s.settimeout(0.1); s.recv(4096)
    except: pass
s.sendall(b"sendkey ret\n"); time.sleep(0.05)
s.close()
PY
sleep 3

# ---- assertions -------------------------------------------------------------------------------
PASS=1
chk() { if grep -q "$1" "$SER" 2>/dev/null; then echo "  OK  : $2"; else echo "  FAIL: $2 (missing: $1)"; PASS=0; fi; }
no()  { if grep -qE "$1" "$SER" 2>/dev/null; then echo "  FAIL: $2"; grep -E "$1" "$SER" | head -3 | sed 's/^/        /'; PASS=0; else echo "  OK  : $2"; fi; }

echo "=== live-USB boot smoke (root on USB mass-storage) ==="
chk "xHCI: "                                  "xHCI controller discovered"
chk "Root: USB mass-storage device"           "root discovery picked the USB volume"
chk "Mounting ext filesystem at /disks/main"  "/disks/main mounted over USB-MSC"
chk "bash-5"                                  "reached the login shell on a USB-only system"
chk "USB_SMOKE_FORK_OK"                        "ring-3 fork/exec from console on the USB root"
no  "CPU EXCEPTION|KERNEL EXCEPTION|killed faulting process|Triple fault"  "no faults on the console"
if grep -qE "Triple fault" "$INT" 2>/dev/null; then echo "  FAIL: QEMU logged a Triple fault"; PASS=0; else echo "  OK  : no triple fault in QEMU int log"; fi

echo "======================================================"
if [ "$PASS" = 1 ]; then echo "live-USB boot smoke: PASS"; exit 0; else echo "live-USB boot smoke: FAIL"; exit 1; fi
