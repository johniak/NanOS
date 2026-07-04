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
# Completion CEILING, not a fixed sleep: the run is polled for the success line every 2 s and
# returns as soon as it appears. nettorture wall-time under MTTCG swings 15 s .. 150+ s with host
# load (Docker Desktop from the image64 rebuild, other QEMUs), so a fixed 150 s sleep flaked while
# a quiet host finished in 15 s. 600 s is generous; a real hang still fails, just later.
SETTLE="${SETTLE:-600}"
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

python3 - "$MON" "$SETTLE" "$SER" <<'PY'
import socket,time,sys
MON, SETTLE, SER = sys.argv[1], float(sys.argv[2]), sys.argv[3]
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
typ("nettorture.nxe", 0.5)
# Poll for completion (success line OR any nettorture verdict/fault) instead of a fixed sleep —
# return the moment the run is decided, wait up to the SETTLE ceiling for a slow host.
deadline = time.time() + SETTLE
while time.time() < deadline:
    time.sleep(2)
    try: log = open(SER, "rb").read().decode(errors="replace")
    except FileNotFoundError: continue
    if ("datagrams ok" in log or "NET RACE" in log or "thread create FAIL" in log
            or "KERNEL EXCEPTION" in log):
        break
PY
sleep 2

echo "=== nettorture output ==="
grep -a "nettorture:" "$SER" | sed 's/\x1b\[[0-9;]*m//g' || true
echo "========================="

if grep -aqE "Kernel panic|PANIC|TRIPLE FAULT|triple fault|KERNEL FAULT|KERNEL EXCEPTION" "$SER"; then
	echo "x86_64 SMP netstress: FAIL — kernel fault/panic in serial log"
	grep -aE "Kernel panic|PANIC|TRIPLE FAULT|triple fault|KERNEL FAULT|KERNEL EXCEPTION" "$SER" | head
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
