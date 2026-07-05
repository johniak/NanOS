#!/usr/bin/env bash
# Drive the NanOS interactive shell headlessly: boot, type each command argument
# (followed by Enter) into the VGA console via QEMU monitor `sendkey`, screendump,
# then quit. Usage: scripts/qemu-shell.sh <out.png> "cmd one" "cmd two" ...
set -u
OUT="${1:?usage: qemu-shell.sh out.png cmd...}"; shift
IMG=disk/image.img
MON=/tmp/nanos-qmon.sock
LOG=/tmp/nanos-int.log
PPM=/tmp/nanos-screen.ppm
rm -f "$MON" "$LOG" "$PPM"

qemu-system-i386 -drive file="$IMG",format=raw \
    -display none -monitor unix:"$MON",server,nowait \
    -no-reboot -d int -D "$LOG" &
QPID=$!
sleep "${BOOT_WAIT:-9}"   # boot to the prompt (override via BOOT_WAIT=secs)

CMDS="$*" python3 - "$MON" "$PPM" "$@" <<'PY'
import socket, sys, time, os
mon, ppm = sys.argv[1], sys.argv[2]
cmds = sys.argv[3:]
s = socket.socket(socket.AF_UNIX); s.connect(mon); time.sleep(0.3); s.recv(65536)

KEYMAP = {' ':'spc','-':'minus','/':'slash','.':'dot',',':'comma','\n':'ret',
          '_':'shift-minus','=':'equal',':':'shift-semicolon',
          "'":'apostrophe','"':'shift-apostrophe'}
def send(cmd):
    s.sendall(cmd.encode()+b"\n"); time.sleep(0.05)
    try: s.recv(65536)
    except: pass

for c in cmds:
    for ch in c:
        if ch.isalnum():
            send("sendkey "+ch)
        elif ch in KEYMAP:
            send("sendkey "+KEYMAP[ch])
        else:
            send("sendkey spc")
        time.sleep(0.12)
    send("sendkey ret")
    time.sleep(0.8)          # let the command run

time.sleep(0.3)
s.sendall(b"screendump %s\n" % ppm.encode()); time.sleep(0.7); s.recv(65536)
s.sendall(b"quit\n"); time.sleep(0.2); s.close()
PY

wait "$QPID" 2>/dev/null
[ -f "$PPM" ] && sips -s format png "$PPM" --out "$OUT" >/dev/null 2>&1 && echo "screendump: $OUT"
for v in "v=08" "v=0d" "v=0e"; do c=$(grep -c "$v" "$LOG" 2>/dev/null); echo "  $v : ${c:-0}"; done
