#!/usr/bin/env bash
# tcpsrv-diag.sh — isolate the SERVER-side TCP path (bind/listen/accept) from inetd. Boots NanOS,
# runs `tcpsrv <port>` in the foreground (it prints each syscall's return value to the console),
# then connects from the host through hostfwd and sends a line. The screendump shows exactly how
# far the passive open got. Usage: scripts/tcpsrv-diag.sh [out.png]
set -u
OUT="${1:-/tmp/nanos-tcpsrv.png}"
IMG=disk/image.img
MON=/tmp/nanos-ts-qmon.sock
LOG=/tmp/nanos-ts-int.log
PPM=/tmp/nanos-ts-screen.ppm
PCAP=/tmp/nanos-ts.pcap
rm -f "$MON" "$LOG" "$PPM" "$PCAP"

qemu-system-i386 -drive file="$IMG",format=raw \
    -netdev user,id=n0,hostfwd=tcp::7777-:7777 -device e1000,netdev=n0 \
    -object filter-dump,id=d0,netdev=n0,file="$PCAP" \
    -display none -monitor unix:"$MON",server,nowait \
    -no-reboot -d int -D "$LOG" &
QPID=$!
sleep "${BOOT_WAIT:-14}"

python3 - "$MON" "$PPM" <<'PY'
import socket, sys, time
mon, ppm = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX); s.connect(mon); time.sleep(0.3); s.recv(65536)
KEYMAP = {' ':'spc','-':'minus','/':'slash','.':'dot',',':'comma','\n':'ret',':':'shift-semicolon'}
def key(k):
    s.sendall(b"sendkey "+k.encode()+b"\n"); time.sleep(0.05)
    try: s.recv(65536)
    except: pass
def typeline(cmd):
    for ch in cmd:
        if ch.isalnum(): key(ch)
        elif ch in KEYMAP: key(KEYMAP[ch])
        else: key('spc')
        time.sleep(0.10)
    key('ret'); time.sleep(0.5)

typeline("tcpsrv 7777")
time.sleep(2.0)   # tcpsrv binds + listens + blocks in accept()

# Connect from the host and send a line; tcpsrv should accept, read, echo, exit.
try:
    c = socket.create_connection(("127.0.0.1", 7777), timeout=5)
    c.settimeout(5)
    c.sendall(b"ping-from-host\n")
    data = b""
    try:
        while len(data) < 64:
            ch = c.recv(64 - len(data))
            if not ch: break
            data += ch
    except socket.timeout: pass
    c.close()
    print("HOST got back:", repr(data))
except Exception as e:
    print("HOST connect error:", e)

time.sleep(1.5)   # let tcpsrv's accept/read/echo prints render
s.sendall(b"screendump %s\n" % ppm.encode()); time.sleep(0.8); s.recv(65536)
s.sendall(b"quit\n"); time.sleep(0.2); s.close()
PY

wait "$QPID" 2>/dev/null
[ -f "$PPM" ] && sips -s format png "$PPM" --out "$OUT" >/dev/null 2>&1 && echo "screendump: $OUT"
echo "=== faults ==="; for v in v=08 v=0d v=0e; do echo "  $v : $(grep -c "$v" "$LOG" 2>/dev/null)"; done
echo "=== pcap (port 7777) ==="; tcpdump -nr "$PCAP" 'tcp port 7777' 2>/dev/null | head -12
