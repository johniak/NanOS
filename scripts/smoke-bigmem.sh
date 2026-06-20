#!/usr/bin/env bash
# smoke-bigmem.sh — boot the image with > 4 GiB RAM (6 GiB straddles the 3-4 GiB PCI hole, so a usable
# mmap region lives above 4 GiB) and assert the 64-bit memory map + huge-page kernel identity map work:
# shell reached, /proc/meminfo MemTotal > 4 GiB, zero faults. Catches the uint32 truncation/wrap.
set -u
IMG=disk/image64-grub2.img
SER=/tmp/nanos-bigmem-serial.log
INT=/tmp/nanos-bigmem-int.log
MON=/tmp/nanos-bigmem-qmon.sock
rm -f "$SER" "$INT" "$MON"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }
pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null
qemu-system-x86_64 -cpu qemu64 -m 6144 -drive file="$IMG",format=raw \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -display none -serial file:"$SER" -monitor unix:"$MON",server,nowait -no-reboot -d int,cpu_reset -D "$INT" &
QPID=$!
trap 'kill -9 "$QPID" 2>/dev/null' EXIT
for i in $(seq 1 45); do grep -q "bash-5" "$SER" 2>/dev/null && break; sleep 1; done
# read /proc/meminfo through the shell via the monitor
python3 - "$MON" <<'PY'
import socket,time,sys
s=socket.socket(socket.AF_UNIX)
try: s.connect(sys.argv[1])
except: sys.exit(0)
time.sleep(0.3)
# QEMU sendkey wants keysym names, not literal chars: space->spc, '/'->slash.
SYM = {' ': 'spc', '/': 'slash'}
for c in "cat /proc/meminfo":
    k = SYM.get(c, c)
    s.sendall(("sendkey "+k+"\n").encode()); time.sleep(0.05)
s.sendall(b"sendkey ret\n"); time.sleep(1)
s.close()
PY
sleep 2
PASS=1
chk(){ if grep -q "$1" "$SER" 2>/dev/null; then echo "  OK  : $2"; else echo "  FAIL: $2 (missing: $1)"; PASS=0; fi; }
no(){ if grep -qE "$1" "$SER" 2>/dev/null; then echo "  FAIL: $2"; PASS=0; else echo "  OK  : $2"; fi; }
echo "=== big-RAM (6 GiB) boot smoke ==="
chk "bash-5"                                          "reached the shell with 6 GiB RAM"
# MemTotal in kB > 4 GiB (4194304 kB). awk picks the MemTotal line's number.
if awk '/MemTotal/{ if ($2+0 > 4194304) ok=1 } END{ exit ok?0:1 }' "$SER" 2>/dev/null; then
    echo "  OK  : /proc/meminfo MemTotal > 4 GiB (high RAM mapped + pooled)"; else
    echo "  FAIL: MemTotal not > 4 GiB"; grep -i MemTotal "$SER" | head -1 | sed 's/^/        /'; PASS=0; fi
no  "CPU EXCEPTION|KERNEL EXCEPTION|Triple fault"     "no faults"
if grep -qE "Triple fault" "$INT" 2>/dev/null; then echo "  FAIL: triple fault in int log"; PASS=0; else echo "  OK  : no triple fault"; fi
echo "================================="
if [ "$PASS" = 1 ]; then echo "big-RAM boot smoke: PASS"; exit 0; else echo "big-RAM boot smoke: FAIL"; exit 1; fi
