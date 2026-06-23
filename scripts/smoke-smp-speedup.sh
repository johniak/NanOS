#!/usr/bin/env bash
# smoke-smp-speedup.sh — the SMP parallel-SPEEDUP gate. Boots the x86_64 image on 4 vCPUs, logs in,
# and runs the SAME fixed total of CPU-bound work as 1 worker thread, then as 4 worker threads
# (user/pfract, integer fixed-point so the result is correct across preemption). The compute loop
# is pure ring-3 (no syscalls), so it runs off the Big Kernel Lock and must get FASTER on 4 cores.
# Asserts both runs produce the SAME checksum (parallel == serial result) and T(1)/T(4) >= a real
# speedup threshold. Modelled on scripts/smoke-x86_64.sh (same proven login drive).
set -u
IMG=disk/image64-grub2.img
SER=/tmp/nanos-smpspeed-serial.log
INT=/tmp/nanos-smpspeed-int.log
MON=/tmp/nanos-smpspeed-qmon.sock
REPEAT="${REPEAT:-1200}"          # grid recomputes; tuned so the 1-thread run is ~seconds
THRESH="${THRESH:-1.8}"           # required T1/T4 speedup on 4 cores
rm -f "$SER" "$INT" "$MON"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }

pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null
# NOTE: no `-d int` here — with four per-CPU LAPIC timers firing 1000 Hz, interrupt logging makes
# QEMU crawl (and the int log explode), so the CPU-bound benchmark would never finish. Speedup
# only needs timing; correctness/fault coverage stays in smoke-smp + smoke-x86_64.
qemu-system-x86_64 -cpu qemu64 -smp 4 -m 512 -drive file="$IMG",format=raw \
    -netdev user,id=n0,hostfwd=tcp::2223-:22 -device e1000,netdev=n0 \
    -display none -serial file:"$SER" -monitor unix:"$MON",server,nowait -no-reboot &
QPID=$!
cleanup() { kill -9 "$QPID" 2>/dev/null; }
trap cleanup EXIT

for i in $(seq 1 40); do grep -q "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done

python3 - "$MON" "$REPEAT" <<'PY'
import socket,time,sys
KM={' ':'spc','.':'dot','\n':'ret'}
def kn(c):
    if c in KM: return KM[c]
    if c.isdigit(): return c
    if c.isalpha(): return ('shift-'+c.lower()) if c.isupper() else c
    return None
s=socket.socket(socket.AF_UNIX)
try: s.connect(sys.argv[1])
except Exception as e: print("monitor connect failed:",e); sys.exit(0)
time.sleep(0.3)
try: s.settimeout(0.3); s.recv(65536)
except: pass
def typ(text, settle):
    for c in text:
        k=kn(c)
        if k: s.sendall(("sendkey "+k+"\n").encode()); time.sleep(0.04)
        try: s.settimeout(0.1); s.recv(4096)
        except: pass
    s.sendall(b"sendkey ret\n"); time.sleep(settle)
R=sys.argv[2]
typ("jan", 1.0)                       # username
typ("jan", 2.5)                       # password -> bash login shell
typ("pfract.nxe 1 "+R, 30.0)          # serial run (1 worker over the whole job)
typ("pfract.nxe 4 "+R, 20.0)          # parallel run (4 workers across 4 cores)
s.close()
PY
sleep 2

# ---- parse + assert ---------------------------------------------------------------------------
echo "=== x86_64 SMP parallel-speedup gate (4 vCPU, repeat=$REPEAT) ==="
grep -aE "pfract: [0-9]+ workers" "$SER" 2>/dev/null | sed 's/^/  /'
get_ms()  { grep -aE "pfract: $1 workers" "$SER" | tail -1 | sed -nE 's/.* ([0-9]+) ms.*/\1/p'; }
get_sum() { grep -aE "pfract: $1 workers" "$SER" | tail -1 | sed -nE 's/.*(checksum=0x[0-9a-f]+).*/\1/p'; }
T1=$(get_ms 1); T4=$(get_ms 4); C1=$(get_sum 1); C4=$(get_sum 4)
PASS=1
if [ -z "$T1" ] || [ -z "$T4" ]; then echo "  FAIL: missing pfract timing (T1='$T1' T4='$T4')"; PASS=0; fi
if [ -n "$C1" ] && [ "$C1" = "$C4" ]; then echo "  OK  : checksums match ($C1) — parallel result == serial"; else echo "  FAIL: checksum mismatch (1=$C1 4=$C4)"; PASS=0; fi
if [ -n "$T1" ] && [ -n "$T4" ] && [ "$T4" -gt 0 ]; then
    SPEEDUP=$(awk "BEGIN{printf \"%.2f\", $T1/$T4}")
    echo "  T(1 thread) = ${T1} ms,  T(4 threads) = ${T4} ms,  speedup = ${SPEEDUP}x"
    OK=$(awk "BEGIN{print ($T1/$T4 >= $THRESH)?1:0}")
    if [ "$OK" = 1 ]; then echo "  OK  : ${SPEEDUP}x >= ${THRESH}x — work ran in parallel on multiple cores"; else echo "  FAIL: speedup ${SPEEDUP}x < ${THRESH}x"; PASS=0; fi
fi
echo "==============================================="
if [ "$PASS" = 1 ]; then echo "x86_64 SMP speedup: PASS"; exit 0; else echo "x86_64 SMP speedup: FAIL"; exit 1; fi
