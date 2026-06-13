#!/usr/bin/env bash
# FAZA 3: TLS server on the guest. Boot NanOS (-cpu Nehalem) with hostfwd 5443->5443, then in the
# guest generate a self-signed cert ON-DEVICE (openssl req -x509, exercises genrsa+sign with the
# CSPRNG), drop an index.html, and run `openssl s_server -WWW`. From the HOST, curl -k the page.
# Proves: on-device keygen/cert + a guest TLS *server* handshake + HTTPS transfer to a host client.
set -u
OUTPNG="${1:-/tmp/tls-server.png}"
IMG=disk/image-grub2.img
MON=/tmp/tls-srv.sock; LOG=/tmp/tls-srv.log; PPM=/tmp/tls-srv.ppm
rm -f "$MON" "$LOG" "$PPM" "$OUTPNG"
qemu-system-i386 -cpu Nehalem -m 512 -snapshot -drive file="$IMG",format=raw \
    -display none -monitor unix:"$MON",server,nowait -no-reboot -d int -D "$LOG" \
    -netdev user,id=n0,hostfwd=tcp::5443-:5443 -device e1000,netdev=n0 &
QPID=$!
sleep "${BOOT_WAIT:-18}"
# Type the guest setup: cd /tmp, make a page, self-sign a cert (slow genrsa), start s_server -WWW &.
python3 - "$MON" <<'PY'
import socket, sys, time
mon = sys.argv[1]
KEYMAP = {' ':'spc','-':'minus','/':'slash','.':'dot',',':'comma','\n':'ret','_':'shift-minus',
          ':':'shift-semicolon','=':'equal','<':'shift-comma','>':'shift-dot'}
s = socket.socket(socket.AF_UNIX); s.connect(mon); time.sleep(0.3); s.recv(65536)
def send(c):
    s.sendall(c.encode()+b"\n"); time.sleep(0.04)
    try: s.recv(65536)
    except: pass
def typ(line, settle):
    for ch in line:
        if ch.isdigit() or (ch.isalpha() and ch.islower()): send("sendkey "+ch)
        elif ch.isalpha(): send("sendkey shift-"+ch.lower())
        elif ch in KEYMAP: send("sendkey "+KEYMAP[ch])
        else: send("sendkey spc")
        time.sleep(0.08)
    send("sendkey ret"); time.sleep(settle)
typ("cd /tmp", 0.6)
typ("echo hello-tls-from-nanos > index.html", 0.6)
typ("openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -subj /CN=nanos -batch", 50.0)
typ("openssl s_server -accept 5443 -cert cert.pem -key key.pem -WWW &", 3.0)
s.close()
PY
echo "=== HOST curl -k https://localhost:5443/index.html ==="
sleep 1
curl -sk --max-time 15 https://localhost:5443/index.html ; echo "(curl exit=$?)"
# screendump the guest console (shows the cert gen + s_server) for the record.
python3 - "$MON" "$PPM" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); time.sleep(0.3); s.recv(65536)
s.sendall(b"screendump %s\n" % sys.argv[2].encode()); time.sleep(0.8); s.recv(65536)
s.sendall(b"quit\n"); time.sleep(0.2); s.close()
PY
wait "$QPID" 2>/dev/null
[ -f "$PPM" ] && sips -s format png "$PPM" --out "$OUTPNG" >/dev/null 2>&1 && echo "screendump: $OUTPNG"
for v in "v=08" "v=0d" "v=0e"; do c=$(grep -c "$v" "$LOG" 2>/dev/null); echo "  $v : ${c:-0}"; done
