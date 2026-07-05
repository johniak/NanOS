#!/usr/bin/env bash
# Headless QEMU boot WITH an e1000 NIC on user-mode NAT, capturing every frame to a pcap and
# decoding it (FAZA 0 of docs/superpowers/plans/2026-06-12-networking.md). This is the
# "no-shortcuts" wire observatory: filter-dump records RX+TX, so we can byte-compare our
# ARP/IP/ICMP/DNS/TCP against what a real Linux emits for the same operation.
#
# Usage: scripts/net-capture.sh [seconds] [out.pcap]
#   - assumes disk/image.img is already built (run `make image` first)
#   - boots headless with the same NIC_OPTS the Makefile's run-net uses (e1000 + filter-dump),
#     monitor on a unix socket, -d int into a log, -no-reboot
#   - after [seconds], screendumps + quits, then decodes the pcap with tcpdump and prints a
#     per-protocol frame summary (ARP / IP / ICMP / UDP:53 DNS / DHCP / TCP)
#   - prints any CPU fault vectors from the int log (v=08 triple / v=0d #GP / v=0e #PF)
set -u
SECS="${1:-8}"
PCAP="${2:-/tmp/nanos.pcap}"
IMG=disk/image.img
MON=/tmp/nanos-net-qmon.sock
LOG=/tmp/nanos-net-int.log
PPM=/tmp/nanos-net-screen.ppm
PNG="${PCAP%.pcap}.png"

rm -f "$MON" "$LOG" "$PPM" "$PCAP"

# hostfwd lets a host client poke a guest server; filter-dump writes the pcap. Keep flags in
# sync with the Makefile NIC_OPTS (single source of wire truth).
qemu-system-i386 -drive file="$IMG",format=raw \
    -netdev user,id=n0,hostfwd=tcp::5555-:80 -device e1000,netdev=n0 \
    -object filter-dump,id=d0,netdev=n0,file="$PCAP" \
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
[ -f "$PPM" ] && sips -s format png "$PPM" --out "$PNG" >/dev/null 2>&1 && echo "screendump: $PNG"

echo "=== pcap: $PCAP ==="
if [ -f "$PCAP" ] && [ -s "$PCAP" ]; then
  total=$(tcpdump -r "$PCAP" 2>/dev/null | wc -l | tr -d ' ')
  echo "  frames captured: $total"
  for f in 'arp:arp' 'icmp:icmp' 'dns:udp port 53' 'dhcp:udp port 67 or udp port 68' 'tcp:tcp' 'ip:ip'; do
    name="${f%%:*}"; filt="${f#*:}"
    c=$(tcpdump -r "$PCAP" "$filt" 2>/dev/null | wc -l | tr -d ' ')
    echo "  $name : $c"
  done
  echo "--- first 40 decoded frames (tcpdump -v) ---"
  tcpdump -nr "$PCAP" -v 2>/dev/null | head -40
else
  echo "  (empty — no frames; NIC not driven yet, expected before FAZA 2)"
fi

echo "=== interrupt-log fault summary ($LOG) ==="
if [ -f "$LOG" ]; then
  for v in "v=08" "v=0d" "v=0e"; do
    c=$(grep -c "$v" "$LOG" 2>/dev/null); echo "  $v : ${c:-0}"
  done
else
  echo "  no log produced"
fi
