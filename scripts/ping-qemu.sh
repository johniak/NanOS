#!/usr/bin/env bash
# Headless QEMU boot that DRIVES the real GNU inetutils ping at the bash prompt and captures the
# wire. Boots with the same e1000 + user-mode NAT + filter-dump as run-net, waits for the shell,
# types a `ping` command via the monitor's sendkey, then screendumps + decodes the pcap. Proves
# the ported /nanos/bin/ping.nxe resolves a name (DNS) and exchanges ICMP echo on the real wire.
#
# Usage: scripts/ping-qemu.sh ["ping args"] [boot_secs] [run_secs] [out.pcap]
set -u
CMD="${1:-ping -c 3 wp.pl}"
BOOT="${2:-14}"
RUN="${3:-9}"
PCAP="${4:-/tmp/nanos-ping.pcap}"
IMG=disk/image-grub2.img
MON=/tmp/nanos-ping-qmon.sock
LOG=/tmp/nanos-ping-int.log
PPM=/tmp/nanos-ping-screen.ppm
PNG="${PCAP%.pcap}.png"

rm -f "$MON" "$LOG" "$PPM" "$PCAP" "$PNG"

qemu-system-i386 -drive file="$IMG",format=raw \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -object filter-dump,id=d0,netdev=n0,file="$PCAP" \
    -display none -monitor unix:"$MON",server,nowait \
    -no-reboot -d int -D "$LOG" &
QPID=$!

BOOT="$BOOT" RUN="$RUN" CMD="$CMD" MON="$MON" PPM="$PPM" python3 - <<'PY'
import socket, sys, time, os
mon, ppm = os.environ["MON"], os.environ["PPM"]
boot, run = float(os.environ["BOOT"]), float(os.environ["RUN"])
cmd = os.environ["CMD"]

# QEMU monitor sendkey names for the characters we need.
KEYMAP = {
    ' ': 'spc', '-': 'minus', '.': 'dot', '\n': 'ret', '_': 'shift-minus',
    '/': 'slash', ':': 'shift-semicolon', '=': 'equal', '~': 'shift-grave_accent',
    '?': 'shift-slash', '&': 'shift-7', ';': 'semicolon',
}
for c in "abcdefghijklmnopqrstuvwxyz0123456789": KEYMAP[c] = c
for c in "abcdefghijklmnopqrstuvwxyz": KEYMAP[c.upper()] = 'shift-' + c

s = socket.socket(socket.AF_UNIX); s.connect(mon); time.sleep(0.3); s.recv(65536)
time.sleep(boot)  # wait for the bash prompt

def key(name):
    s.sendall(b"sendkey %s\n" % name.encode()); time.sleep(0.05); s.recv(65536); time.sleep(0.05)

for ch in cmd:
    k = KEYMAP.get(ch)
    if k is None:
        sys.stderr.write("no keymap for %r\n" % ch); continue
    key(k)
key('ret')

time.sleep(run)  # let ping resolve + echo
s.sendall(b"screendump %s\n" % ppm.encode()); time.sleep(0.8); s.recv(65536)
s.sendall(b"quit\n"); time.sleep(0.2); s.close()
PY

wait "$QPID" 2>/dev/null
[ -f "$PPM" ] && sips -s format png "$PPM" --out "$PNG" >/dev/null 2>&1 && echo "screendump: $PNG"

echo "=== pcap: $PCAP ==="
if command -v tcpdump >/dev/null 2>&1; then
    echo "  ARP   : $(tcpdump -nr "$PCAP" arp 2>/dev/null | wc -l | tr -d ' ')"
    echo "  DNS   : $(tcpdump -nr "$PCAP" 'udp port 53' 2>/dev/null | wc -l | tr -d ' ')"
    echo "  ICMP  : $(tcpdump -nr "$PCAP" icmp 2>/dev/null | wc -l | tr -d ' ')"
    echo "--- ICMP frames ---"; tcpdump -nr "$PCAP" icmp 2>/dev/null | head -20
    echo "--- DNS frames ---";  tcpdump -nr "$PCAP" 'udp port 53' 2>/dev/null | head -8
fi
echo "=== faults (v=08 triple / v=0d #GP / v=0e #PF) ==="
for v in 08 0d 0e; do echo "  v=$v : $(grep -c "v=$v" "$LOG" 2>/dev/null)"; done
