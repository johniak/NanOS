#!/usr/bin/env bash
# FAZA 0 non-determinism gate: boot NanOS twice (with -cpu Nehalem so RDRAND is exposed), run
# `randhex` in each boot, screendump both. The getentropy/urandom hex lines must DIFFER between
# the two boots — proof the kernel CSPRNG is seeded from real entropy, not a fixed seed.
# Usage: scripts/randhex-qemu.sh   (writes /tmp/randhex-bootN.png; -d int fault check per boot)
set -u
IMG=disk/image-grub2.img
boot_once() {
    local tag="$1"
    local MON="/tmp/nanos-qmon-$tag.sock"
    local LOG="/tmp/nanos-int-$tag.log"
    local PPM="/tmp/nanos-screen-$tag.ppm"
    local OUT="/tmp/randhex-$tag.png"
    rm -f "$MON" "$LOG" "$PPM" "$OUT"
    qemu-system-i386 -cpu Nehalem -m 512 -snapshot -drive file="$IMG",format=raw \
        -display none -monitor unix:"$MON",server,nowait \
        -no-reboot -d int -D "$LOG" &
    local QPID=$!
    sleep "${BOOT_WAIT:-9}"
    python3 - "$MON" "$PPM" <<'PY'
import socket, sys, time
mon, ppm = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX); s.connect(mon); time.sleep(0.3); s.recv(65536)
def send(c):
    s.sendall(c.encode()+b"\n"); time.sleep(0.05)
    try: s.recv(65536)
    except: pass
for ch in "randhex":
    send("sendkey "+ch); time.sleep(0.12)
send("sendkey ret"); time.sleep(1.2)
s.sendall(b"screendump %s\n" % ppm.encode()); time.sleep(0.7); s.recv(65536)
s.sendall(b"quit\n"); time.sleep(0.2); s.close()
PY
    wait "$QPID" 2>/dev/null
    [ -f "$PPM" ] && sips -s format png "$PPM" --out "$OUT" >/dev/null 2>&1 && echo "screendump: $OUT"
    for v in "v=08" "v=0d" "v=0e"; do c=$(grep -c "$v" "$LOG" 2>/dev/null); echo "  $tag $v : ${c:-0}"; done
}
boot_once boot1
boot_once boot2
echo "compare /tmp/randhex-boot1.png vs /tmp/randhex-boot2.png — the hex lines must differ"
