#!/usr/bin/env bash
# FAZA 2.4: boot NanOS (-cpu Nehalem), fetch an HTTPS URL with wget (built --with-ssl=openssl)
# over slirp, screendump. Proves DNS -> TCP -> TLS (cert-verified) -> HTTP 200 -> file saved on
# the guest. Usage: wget-https-qemu.sh [out.png] [url]
set -u
OUT="${1:-/tmp/wget-https.png}"
URL="${2:-https://example.com/}"
IMG=disk/image.img
MON=/tmp/wget-https.sock; LOG=/tmp/wget-https.log; PPM=/tmp/wget-https.ppm
rm -f "$MON" "$LOG" "$PPM" "$OUT"
qemu-system-i386 -cpu Nehalem -m 512 -snapshot -drive file="$IMG",format=raw \
    -display none -monitor unix:"$MON",server,nowait -no-reboot -d int -D "$LOG" \
    -netdev user,id=n0 -device e1000,netdev=n0 &
QPID=$!
sleep "${BOOT_WAIT:-18}"
# cd into the writable /tmp tmpfs first (the cwd is read-only), then fetch — wget saves
# index.html there. All lowercase so QEMU sendkey needs no shifting beyond the keymap.
URL="$URL" python3 - "$MON" "$PPM" <<'PY'
import socket, sys, time, os
mon, ppm = sys.argv[1], sys.argv[2]
url = os.environ["URL"]
KEYMAP = {' ':'spc','-':'minus','/':'slash','.':'dot',',':'comma','\n':'ret','_':'shift-minus',
          ':':'shift-semicolon','<':'shift-comma','>':'shift-dot'}
s = socket.socket(socket.AF_UNIX); s.connect(mon); time.sleep(0.3); s.recv(65536)
def send(c):
    s.sendall(c.encode()+b"\n"); time.sleep(0.04)
    try: s.recv(65536)
    except: pass
def typ(line, settle):
    for ch in line:
        if ch.isdigit() or (ch.isalpha() and ch.islower()): send("sendkey "+ch)
        elif ch.isalpha(): send("sendkey shift-"+ch.lower())   # uppercase -> shifted key
        elif ch in KEYMAP: send("sendkey "+KEYMAP[ch])
        else: send("sendkey spc")
        time.sleep(0.09)
    send("sendkey ret"); time.sleep(settle)
typ("cd /tmp", 0.6)
typ("wget " + url, 11.0)             # DNS + TCP + TLS + HTTP fetch
typ("ls -l index.html", 1.5)         # prove the file landed
s.sendall(b"screendump %s\n" % ppm.encode()); time.sleep(0.8); s.recv(65536)
s.sendall(b"quit\n"); time.sleep(0.2); s.close()
PY
wait "$QPID" 2>/dev/null
[ -f "$PPM" ] && sips -s format png "$PPM" --out "$OUT" >/dev/null 2>&1 && echo "screendump: $OUT"
for v in "v=08" "v=0d" "v=0e"; do c=$(grep -c "$v" "$LOG" 2>/dev/null); echo "  $v : ${c:-0}"; done
