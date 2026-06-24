#!/usr/bin/env bash
# smoke-smp-netstress.sh — the SMP NETWORK data-race gate for retiring the BKL (Phase 4, Task 15e).
#
# Boots the x86_64 image on 4 vCPUs under MTTCG, logs in, and runs user/nettorture — NT threads
# ping-ponging UDP datagrams to themselves over loopback. Each datagram crosses the RX-softirq
# thread, so that kernel thread, the net-timer thread, and the socket syscalls all hit the net
# stack at once on different cores. nettorture's oracle is deterministic (each thread verifies its
# own datagram comes back byte-for-byte before sending the next), so a misdelivery / corruption /
# lost wakeup makes the success line never print. Required before flipping the BKL off (15f).
set -u
IMG=disk/image64-grub2.img
SER=/tmp/nanos-smpnet.log
MON=/tmp/nanos-smpnet-qmon.sock
SETTLE="${SETTLE:-90}"
rm -f "$SER" "$MON"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }

pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null
qemu-system-x86_64 -accel tcg,thread=multi -cpu qemu64 -smp 4 -m 512 -drive file="$IMG",format=raw \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -display none -serial file:"$SER" -monitor unix:"$MON",server,nowait -no-reboot &
QPID=$!
cleanup() { kill -9 "$QPID" 2>/dev/null; }
trap cleanup EXIT

for i in $(seq 1 50); do grep -q "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done
if ! grep -q "nanos login:" "$SER" 2>/dev/null; then
	echo "FAIL: never reached login"; tail -20 "$SER" 2>/dev/null; exit 1
fi

python3 - "$MON" "$SETTLE" <<'PY'
import socket,time,sys
MON, SETTLE = sys.argv[1], float(sys.argv[2])
def kn(c):
    if c.isdigit() or c.isalpha(): return c
    return {' ':'spc','.':'dot','\n':'ret','/':'slash'}.get(c)
def fresh():
    s=socket.socket(socket.AF_UNIX)
    try: s.connect(MON)
    except Exception as e: print("monitor connect failed:",e); return None
    time.sleep(0.2)
    try: s.settimeout(0.3); s.recv(65536)
    except: pass
    return s
def typ(text, settle):
    s=fresh()
    if not s: return
    for c in text:
        k=kn(c)
        if k: s.sendall(("sendkey "+k+"\n").encode()); time.sleep(0.05)
    s.sendall(b"sendkey ret\n"); time.sleep(settle); s.close()
typ("jan", 1.5)
typ("jan", 2.5)
typ("nettorture.nxe", SETTLE)
PY
sleep 2

echo "=== nettorture output ==="
grep -a "nettorture:" "$SER" | sed 's/\x1b\[[0-9;]*m//g' || true
echo "========================="

if grep -aqE "Kernel panic|PANIC|TRIPLE FAULT|triple fault|KERNEL FAULT" "$SER"; then
	echo "x86_64 SMP netstress: FAIL — kernel fault/panic in serial log"
	grep -aE "Kernel panic|PANIC|TRIPLE FAULT|triple fault|KERNEL FAULT" "$SER" | head
	exit 1
fi
if grep -aq "nettorture: .* datagrams ok" "$SER"; then
	echo "x86_64 SMP netstress: PASS"
	exit 0
fi
if grep -aq "nettorture:" "$SER"; then
	echo "x86_64 SMP netstress: FAIL — nettorture reported a race/oracle failure (see output above)"
	exit 1
fi
echo "x86_64 SMP netstress: FAIL — no nettorture output (hang? crash? lost the drive)"
tail -20 "$SER" 2>/dev/null
exit 1
