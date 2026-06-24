#!/usr/bin/env bash
# smoke-smp-stress.sh — the SMP DATA-RACE gate for retiring the Big Kernel Lock (Phase 4, Task 15).
#
# Boots the x86_64 image on 4 vCPUs under MTTCG (so threads genuinely run on different host threads
# in parallel), logs in, and runs user/smptorture — which hammers the per-process FD table, a shared
# pipe ring, signal delivery, and a mutex-guarded counter from 8 threads at once. smptorture's
# oracles are order-independent and deterministic: a lost/duplicated pipe byte, a cross-thread fd
# corruption, or a torn counter makes its single success line never print. This gate therefore
# actually EXERCISES the races the fine-grained locks (15b-15e) protect — unlike the boot/ping/usb
# smokes, which never collide on those structures. Required before flipping the BKL off (15f).
#
# Pass iff the success line "smptorture: ... ok" appears and no race/fault/panic marker does.
set -u
IMG=disk/image64-grub2.img
SER=/tmp/nanos-smpstress.log
MON=/tmp/nanos-smpstress-qmon.sock
SETTLE="${SETTLE:-150}"           # seconds to let the torture run (TCG is slow; it is deterministic)
rm -f "$SER" "$MON"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }

pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null
# MTTCG (-accel tcg,thread=multi) is REQUIRED: single-threaded TCG round-robins all vCPUs on one
# host thread, so the threads never truly overlap and a race would not reproduce. MTTCG gives each
# vCPU its own host thread, so collisions on the shared kernel structures actually happen.
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
typ("jan", 1.5)                  # username
typ("jan", 2.5)                  # password -> bash login shell
typ("smptorture.nxe", SETTLE)    # the race gate
PY
sleep 2

echo "=== smptorture output ==="
grep -a "smptorture:" "$SER" | sed 's/\x1b\[[0-9;]*m//g' || true
echo "========================="

# Kernel-level failure: a panic or a triple/GP/page fault logged by the kernel itself (NOT the
# benign "[FAILED]" kext lines for NICs absent in QEMU). Keep this marker list tight.
if grep -aqE "Kernel panic|PANIC|TRIPLE FAULT|triple fault|KERNEL FAULT" "$SER"; then
	echo "x86_64 SMP stress: FAIL — kernel fault/panic in serial log"
	grep -aE "Kernel panic|PANIC|TRIPLE FAULT|triple fault|KERNEL FAULT" "$SER" | head
	exit 1
fi
# Decision is driven by smptorture's OWN output: the success line ends "... signals ok"; any other
# "smptorture:" line is a self-reported race/oracle failure; no line at all means it hung or crashed.
if grep -aq "smptorture: .* signals ok" "$SER"; then
	echo "x86_64 SMP stress: PASS"
	exit 0
fi
if grep -aq "smptorture:" "$SER"; then
	echo "x86_64 SMP stress: FAIL — smptorture reported a race/oracle failure (see output above)"
	exit 1
fi
echo "x86_64 SMP stress: FAIL — no smptorture output (hang? crash? lost the drive)"
tail -20 "$SER" 2>/dev/null
exit 1
