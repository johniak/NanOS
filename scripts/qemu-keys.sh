#!/usr/bin/env bash
# Drive the NanOS console headlessly with arbitrary key steps, including Ctrl combos.
# Boots disk/image-grub2.img, replays each step into the VGA console via QEMU monitor
# `sendkey`, screendumps to <out.png>, then quits and prints a fault summary.
#
# Usage: scripts/qemu-keys.sh <out.png> <boot-secs> step [step ...]
#   step forms:
#     "cmd args"   type the string (char by char) then Enter
#     key:NAME     send a raw QEMU key (e.g. key:ctrl-c, key:ctrl-z, key:ret)
#     wait:N       sleep N seconds (fractional ok)
# Assumes `make image` already ran with grub timeout=0.
set -u
OUT="${1:?usage: qemu-keys.sh out.png boot-secs step...}"; shift
BOOT="${1:?boot-secs}"; shift
IMG=disk/image-grub2.img
MON=/tmp/nanos-qmon.sock
LOG=/tmp/nanos-int.log
PPM=/tmp/nanos-screen.ppm
rm -f "$MON" "$LOG" "$PPM"

qemu-system-i386 -drive file="$IMG",format=raw \
    -display none -monitor unix:"$MON",server,nowait \
    -no-reboot -d int -D "$LOG" &
QPID=$!
sleep "$BOOT"

python3 - "$MON" "$PPM" "$BOOT" "$@" <<'PY'
import socket, sys, time
mon, ppm = sys.argv[1], sys.argv[2]
steps = sys.argv[4:]
s = socket.socket(socket.AF_UNIX); s.connect(mon); time.sleep(0.3)
try: s.recv(65536)
except: pass

KEYMAP = {' ':'spc','-':'minus','/':'slash','.':'dot',',':'comma','\n':'ret',
          '_':'shift-minus'}
def mon_send(line):
    s.sendall(line.encode()+b"\n"); time.sleep(0.05)
    try: s.recv(65536)
    except: pass

def typestr(cmd):
    for ch in cmd:
        if ch.isalnum():
            mon_send("sendkey "+ch)
        elif ch in KEYMAP:
            mon_send("sendkey "+KEYMAP[ch])
        else:
            mon_send("sendkey spc")
        time.sleep(0.10)
    mon_send("sendkey ret")
    time.sleep(0.8)

for step in steps:
    if step.startswith("key:"):
        mon_send("sendkey "+step[4:]); time.sleep(0.6)
    elif step.startswith("wait:"):
        time.sleep(float(step[5:]))
    else:
        typestr(step)

time.sleep(0.3)
s.sendall(b"screendump %s\n" % ppm.encode()); time.sleep(0.7)
try: s.recv(65536)
except: pass
s.sendall(b"quit\n"); time.sleep(0.2); s.close()
PY

wait "$QPID" 2>/dev/null
[ -f "$PPM" ] && sips -s format png "$PPM" --out "$OUT" >/dev/null 2>&1 && echo "screendump: $OUT"
echo "=== fault summary ($LOG) ==="
for v in "v=08" "v=0d" "v=0e"; do c=$(grep -c "$v" "$LOG" 2>/dev/null); echo "  $v : ${c:-0}"; done
c=$(grep -c 'v=80' "$LOG" 2>/dev/null); echo "  (syscall) v=80 : ${c:-0}"
