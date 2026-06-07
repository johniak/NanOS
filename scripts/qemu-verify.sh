#!/usr/bin/env bash
# Headless QEMU boot + screendump + interrupt-log capture (macOS host).
#
# Usage: scripts/qemu-verify.sh <seconds> <out.png>
#   - assumes disk/image-grub2.img is already built (run `make image` first)
#   - boots with grub timeout already 0 (caller sets it), monitor on a unix socket,
#     -d int into /tmp/nanos-int.log, -no-reboot (so a triple fault stops, not loops)
#   - after <seconds>, screendumps to <out.png> (via sips from PPM) and quits cleanly
#   - prints a summary of fault vectors (v=08 triple, v=0d #GP, v=0e #PF) from the log
set -u
SECS="${1:-4}"
OUT="${2:-/tmp/nanos.png}"
IMG=disk/image-grub2.img
MON=/tmp/nanos-qmon.sock
LOG=/tmp/nanos-int.log
PPM=/tmp/nanos-screen.ppm

rm -f "$MON" "$LOG" "$PPM"
qemu-system-i386 -drive file="$IMG",format=raw \
    -display none -monitor unix:"$MON",server,nowait \
    -no-reboot -d int -D "$LOG" &
QPID=$!

sleep "$SECS"

python3 - "$MON" "$PPM" <<'PY'
import socket, sys, time
mon, ppm = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX); s.connect(mon); time.sleep(0.2)
s.recv(65536)
s.sendall(b"screendump %s\n" % ppm.encode()); time.sleep(0.6)
s.recv(65536)
s.sendall(b"quit\n"); time.sleep(0.2)
s.close()
PY

wait "$QPID" 2>/dev/null
[ -f "$PPM" ] && sips -s format png "$PPM" --out "$OUT" >/dev/null 2>&1 && echo "screendump: $OUT"

echo "=== interrupt-log fault summary ($LOG) ==="
if [ -f "$LOG" ]; then
  for v in "v=08" "v=0d" "v=0e"; do
    c=$(grep -c "$v" "$LOG" 2>/dev/null); echo "  $v : ${c:-0}"
  done
  c=$(grep -c 'v=80' "$LOG" 2>/dev/null); echo "  (syscall) v=80 occurrences: ${c:-0}"
else
  echo "  no log produced"
fi
