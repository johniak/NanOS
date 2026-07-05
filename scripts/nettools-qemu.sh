#!/usr/bin/env bash
# nettools-qemu.sh — FAZA I: boot NanOS with a NIC (DHCP), type each diagnostic command on the
# console and screendump after each. Default commands exercise ifconfig (live DHCP config) and
# traceroute (UDP TTL + ICMP time-exceeded -> FAZA C). Screens land at <base>-1.png, -2.png, ...
# Usage: scripts/nettools-qemu.sh [base.png] [ "cmd1" "cmd2" ... ]
set -u
BASE="${1:-/tmp/nanos-nettools.png}"; shift || true
CMDS=( "$@" )
[ ${#CMDS[@]} -eq 0 ] && CMDS=( "ifconfig" "traceroute -q 1 -w 2 -m 4 10.0.2.2" )
IMG=disk/image.img
MON=/tmp/nanos-nt-qmon.sock
LOG=/tmp/nanos-nt-int.log
PCAP=/tmp/nanos-nt.pcap
rm -f "$MON" "$LOG" "$PCAP"

qemu-system-i386 -drive file="$IMG",format=raw \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -object filter-dump,id=d0,netdev=n0,file="$PCAP" \
    -display none -monitor unix:"$MON",server,nowait -no-reboot -d int -D "$LOG" &
QPID=$!
sleep "${BOOT_WAIT:-15}"

BASE="$BASE" python3 - "$MON" "${CMDS[@]}" <<'PY'
import socket, sys, time, os
mon = sys.argv[1]; cmds = sys.argv[2:]
base = os.environ["BASE"]
s = socket.socket(socket.AF_UNIX); s.connect(mon); time.sleep(0.3); s.recv(65536)
KEYMAP={' ':'spc','-':'minus','/':'slash','.':'dot',',':'comma','\n':'ret','_':'shift-minus',
        '=':'equal',':':'shift-semicolon','&':'shift-7'}
def key(k):
    s.sendall(b"sendkey "+k.encode()+b"\n"); time.sleep(0.05)
    try: s.recv(65536)
    except: pass
def typeline(cmd):
    for ch in cmd:
        if ch.isalnum(): key(ch)
        elif ch in KEYMAP: key(KEYMAP[ch])
        else: key('spc')
        time.sleep(0.09)
    key('ret')
def shot(path):
    s.sendall(b"screendump %s\n"%path.encode()); time.sleep(0.7)
    try: s.recv(65536)
    except: pass

for i,c in enumerate(cmds, 1):
    typeline(c); time.sleep(3.0)
    ppm = "/tmp/nanos-nt-%d.ppm"%i
    shot(ppm)
    os.system("sips -s format png %s --out %s-%d.png >/dev/null 2>&1" % (ppm, base.rsplit('.',1)[0], i))
    print("screen %d -> %s-%d.png  (cmd: %s)" % (i, base.rsplit('.',1)[0], i, c))
s.sendall(b"quit\n"); time.sleep(0.2); s.close()
PY

wait "$QPID" 2>/dev/null
echo "=== faults ==="; for v in v=08 v=0d v=0e; do echo "  $v : $(grep -c "$v" "$LOG" 2>/dev/null)"; done
echo "=== pcap (UDP + ICMP, traceroute) ==="
tcpdump -nr "$PCAP" 'icmp or udp' 2>/dev/null | head -16
