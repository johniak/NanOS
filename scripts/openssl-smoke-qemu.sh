#!/usr/bin/env bash
# FAZA 1.3 OpenSSL smoke: boot NanOS (-cpu Nehalem for RDRAND), run a few openssl CLI commands,
# screendump. Proves the ported openssl runs: version, rand -hex (CSPRNG, differs per call),
# dgst -sha256 (compare to the host sum of the same file). Usage: openssl-smoke-qemu.sh out.png
set -u
OUT="${1:-/tmp/openssl-smoke.png}"
IMG=disk/image.img
MON=/tmp/ossl-smoke.sock; LOG=/tmp/ossl-smoke.log; PPM=/tmp/ossl-smoke.ppm
rm -f "$MON" "$LOG" "$PPM" "$OUT"
qemu-system-i386 -cpu Nehalem -m 512 -snapshot -drive file="$IMG",format=raw \
    -display none -monitor unix:"$MON",server,nowait -no-reboot -d int -D "$LOG" &
QPID=$!
sleep "${BOOT_WAIT:-17}"
python3 - "$MON" "$PPM" <<'PY'
import socket, sys, time
mon, ppm = sys.argv[1], sys.argv[2]
cmds = ["openssl version",
        "openssl rand -hex 16",
        "openssl dgst -sha256 /disks/main/nanos/config/passwd",
        "openssl genrsa 2048"]
KEYMAP = {' ':'spc','-':'minus','/':'slash','.':'dot',',':'comma','\n':'ret','_':'shift-minus'}
s = socket.socket(socket.AF_UNIX); s.connect(mon); time.sleep(0.3); s.recv(65536)
def send(c):
    s.sendall(c.encode()+b"\n"); time.sleep(0.04)
    try: s.recv(65536)
    except: pass
for cmd in cmds:
    for ch in cmd:
        if ch.isalnum(): send("sendkey "+ch)
        elif ch in KEYMAP: send("sendkey "+KEYMAP[ch])
        else: send("sendkey spc")
        time.sleep(0.10)
    send("sendkey ret")
    time.sleep(20.0 if "genrsa" in cmd else 2.0)   # RSA keygen (no-asm) is slow in QEMU
time.sleep(0.5)
s.sendall(b"screendump %s\n" % ppm.encode()); time.sleep(0.8); s.recv(65536)
s.sendall(b"quit\n"); time.sleep(0.2); s.close()
PY
wait "$QPID" 2>/dev/null
[ -f "$PPM" ] && sips -s format png "$PPM" --out "$OUT" >/dev/null 2>&1 && echo "screendump: $OUT"
for v in "v=08" "v=0d" "v=0e"; do c=$(grep -c "$v" "$LOG" 2>/dev/null); echo "  $v : ${c:-0}"; done
