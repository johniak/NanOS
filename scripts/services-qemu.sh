#!/usr/bin/env bash
# services-qemu.sh — FAZA H acceptance harness: boot NanOS headless with the inetd super-server
# reachable from the host through QEMU slirp hostfwd, then drive it from the macOS host.
#
# Flow: boot (DHCP brings eth0 up) -> start inetd daemonized at the bash prompt -> from the HOST
# connect to the forwarded ports (daytime tcp 13 -> host 5013, echo tcp 7 -> host 5007) and check
# the replies -> cat /proc/net/tcp (LISTEN sockets) -> screendump -> quit, report CPU faults.
#
# Usage: scripts/services-qemu.sh [out.png]
set -u
OUT="${1:-/tmp/nanos-services.png}"
IMG=disk/image-grub2.img
MON=/tmp/nanos-svc-qmon.sock
LOG=/tmp/nanos-svc-int.log
PPM=/tmp/nanos-svc-screen.ppm
PCAP=/tmp/nanos-svc.pcap
rm -f "$MON" "$LOG" "$PPM" "$PCAP"

# Same NIC + hostfwd map as the Makefile NIC_OPTS (single source of wire truth).
qemu-system-i386 -drive file="$IMG",format=raw \
    -netdev user,id=n0,hostfwd=tcp::5555-:80,hostfwd=tcp::2323-:23,hostfwd=tcp::5007-:7,hostfwd=tcp::5013-:13 \
    -device e1000,netdev=n0 \
    -object filter-dump,id=d0,netdev=n0,file="$PCAP" \
    -display none -monitor unix:"$MON",server,nowait \
    -no-reboot -d int -D "$LOG" &
QPID=$!
sleep "${BOOT_WAIT:-14}"   # boot to the prompt + DHCP lease (override via BOOT_WAIT=secs)

# Type the inetd command at the console, then run the host-side probes from inside python.
python3 - "$MON" "$PPM" <<'PY'
import socket, sys, time
mon, ppm = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX); s.connect(mon); time.sleep(0.3); s.recv(65536)

KEYMAP = {' ':'spc','-':'minus','/':'slash','.':'dot',',':'comma','\n':'ret',
          '_':'shift-minus','=':'equal',':':'shift-semicolon',
          "'":'apostrophe','"':'shift-apostrophe','&':'shift-7'}
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

# Start inetd daemonized (the production path: daemon() forks + setsid + serves detached). Long
# options + the pidfile in writable /tmp avoid the short-opt getopt quirk and the RO-pidfile warn.
typeline("inetd --pidfile=/tmp/inetd.pid /disks/main/nanos/config/etc/inetd.conf")
time.sleep(2.5)   # let inetd parse the conf + bind/listen on every service
typeline("cat /proc/net/tcp")
time.sleep(1.0)

def probe(port, send=b"", read_n=128, timeout=4.0):
    try:
        c = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        c.settimeout(timeout)
        if send: c.sendall(send)
        data = b""
        try:
            while len(data) < read_n:
                chunk = c.recv(read_n - len(data))
                if not chunk: break
                data += chunk
        except socket.timeout:
            pass
        c.close()
        return data
    except Exception as e:
        return ("ERR:%s" % e).encode()

day = probe(5013, b"", 64)            # daytime: server sends the date, then closes
ech = probe(5007, b"hello\n", 6)      # echo: server bounces our bytes back
print("DAYTIME(13):", repr(day))
print("ECHO(7):    ", repr(ech))

# give the console a moment to render inetd's debug, then capture + quit
time.sleep(1.0)
s.sendall(b"screendump %s\n" % ppm.encode()); time.sleep(0.8); s.recv(65536)
s.sendall(b"quit\n"); time.sleep(0.2); s.close()
PY

wait "$QPID" 2>/dev/null
[ -f "$PPM" ] && sips -s format png "$PPM" --out "$OUT" >/dev/null 2>&1 && echo "screendump: $OUT"
echo "=== CPU fault vectors in $LOG ==="
for v in "v=08" "v=0d" "v=0e"; do c=$(grep -c "$v" "$LOG" 2>/dev/null); echo "  $v : ${c:-0}"; done
echo "=== pcap: $PCAP (tcp to ports 7/13) ==="
tcpdump -nr "$PCAP" 'tcp' 2>/dev/null | grep -E '\.(7|13|2323):|\.(7|13):' | head -20
