#!/usr/bin/env bash
# FAZA 2 TLS client: boot NanOS (-cpu Nehalem), run `openssl s_client` to a real HTTPS host over
# slirp NAT, screendump the handshake summary. Proves a real TLS handshake + (with a CA bundle)
# certificate verification from the guest. Usage: openssl-tls-qemu.sh [out.png] [host]
set -u
OUT="${1:-/tmp/openssl-tls.png}"
HOST="${2:-example.com}"
IMG=disk/image-grub2.img
MON=/tmp/ossl-tls.sock; LOG=/tmp/ossl-tls.log; PPM=/tmp/ossl-tls.ppm
rm -f "$MON" "$LOG" "$PPM" "$OUT"
qemu-system-i386 -cpu Nehalem -m 512 -snapshot -drive file="$IMG",format=raw \
    -display none -monitor unix:"$MON",server,nowait -no-reboot -d int -D "$LOG" \
    -netdev user,id=n0 -device e1000,netdev=n0 &
QPID=$!
sleep "${BOOT_WAIT:-18}"
CMD="openssl s_client -connect $HOST:443 -servername $HOST -brief </dev/null"
CMD="$CMD" python3 - "$MON" "$PPM" <<'PY'
import socket, sys, time, os
mon, ppm = sys.argv[1], sys.argv[2]
cmd = os.environ["CMD"]
KEYMAP = {' ':'spc','-':'minus','/':'slash','.':'dot',',':'comma','\n':'ret','_':'shift-minus',
          ':':'shift-semicolon','<':'shift-comma','>':'shift-dot'}
s = socket.socket(socket.AF_UNIX); s.connect(mon); time.sleep(0.3); s.recv(65536)
def send(c):
    s.sendall(c.encode()+b"\n"); time.sleep(0.04)
    try: s.recv(65536)
    except: pass
for ch in cmd:
    if ch.isalnum(): send("sendkey "+ch)
    elif ch in KEYMAP: send("sendkey "+KEYMAP[ch])
    else: send("sendkey spc")
    time.sleep(0.09)
send("sendkey ret"); time.sleep(8.0)   # DNS + TCP + TLS handshake over slirp
s.sendall(b"screendump %s\n" % ppm.encode()); time.sleep(0.8); s.recv(65536)
s.sendall(b"quit\n"); time.sleep(0.2); s.close()
PY
wait "$QPID" 2>/dev/null
[ -f "$PPM" ] && sips -s format png "$PPM" --out "$OUT" >/dev/null 2>&1 && echo "screendump: $OUT"
for v in "v=08" "v=0d" "v=0e"; do c=$(grep -c "$v" "$LOG" 2>/dev/null); echo "  $v : ${c:-0}"; done
