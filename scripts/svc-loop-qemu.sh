#!/usr/bin/env bash
# svc-loop-qemu.sh — FAZA H5 leak check: hammer the boot-time httpd with many sequential GETs,
# then confirm the server still answers and that connections are not piling up (TCBs freed).
# A lighter stand-in for the "10 min idle + 100 connections" soak: if close/cleanup leaked a TCB
# or netbuf per connection, the run would degrade and /proc/net/tcp would accumulate dead rows.
# Usage: scripts/svc-loop-qemu.sh [N]   (default 40 GETs)
set -u
N="${1:-40}"
IMG=disk/image.img
MON=/tmp/nanos-loop-qmon.sock
LOG=/tmp/nanos-loop-int.log
PPM=/tmp/nanos-loop-screen.ppm
rm -f "$MON" "$LOG" "$PPM"

qemu-system-i386 -drive file="$IMG",format=raw \
    -netdev user,id=n0,hostfwd=tcp::5555-:80 -device e1000,netdev=n0 \
    -display none -monitor unix:"$MON",server,nowait -no-reboot -d int -D "$LOG" &
QPID=$!
sleep "${BOOT_WAIT:-16}"

N="$N" python3 - "$MON" "$PPM" <<'PY'
import socket, sys, time, os
mon, ppm = sys.argv[1], sys.argv[2]
N = int(os.environ["N"])
m = socket.socket(socket.AF_UNIX); m.connect(mon); time.sleep(0.3); m.recv(65536)

def get():
    try:
        c=socket.create_connection(("127.0.0.1",5555),timeout=5); c.settimeout(5)
        c.sendall(b"GET / HTTP/1.0\r\nHost: x\r\n\r\n")
        data=b""
        while True:
            d=c.recv(4096)
            if not d: break
            data+=d
        c.close()
        return b"200" in data.split(b"\r\n",1)[0] and b"NanOS" in data
    except Exception:
        return False

ok=0
for i in range(N):
    if get(): ok+=1
print("sequential GETs: %d/%d returned 200 + body" % (ok, N))
print("server still alive after the loop:", get())

time.sleep(0.3)
m.sendall(b"screendump %s\n" % ppm.encode()); time.sleep(0.8); m.recv(65536)
m.sendall(b"quit\n"); time.sleep(0.2); m.close()
PY

wait "$QPID" 2>/dev/null
echo "=== faults ==="; for v in v=08 v=0d v=0e; do echo "  $v : $(grep -c "$v" "$LOG" 2>/dev/null)"; done
