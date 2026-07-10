#!/usr/bin/env bash
# smoke-fpu.sh — FPU/SSE context-switch integrity gate.
#
# Ring 3 is SSE-ON; the kernel is -mno-sse. archContextSwitch historically switched only the
# callee-saved GPRs + CR3 — XMM/MXCSR state LEAKED between tasks on every deferred preemption
# (silent data corruption; the Dell's "impossible" SIGSEGVs inside a fuzz-proven PNG decoder).
# Fixed with per-task FXSAVE areas in the switch path. This gate runs user/fputorture.c:
# 4 forked workers park unique patterns in xmm8-11 + a unique MXCSR rounding mode, busy-spin
# through timer preemptions at -smp 4, and verify the registers survived.
#
# Phase 2 gates the SIGNAL path of the same class: a SIGALRM handler hostile-clobbers
# xmm8-11 + MXCSR while the loop runs. It flushed out (and now guards) FIVE sigframe bugs:
# no FPU state in the frame, handler-entry rsp misalignment (compiled movaps spills #GP'd),
# red-zone clobber of the interrupted leaf frame, sysret eating rcx/r11 on sigreturn (fixed
# with an iretq exit), and sigreturn truncating the restored rax to 32 bits via the int-typed
# syscall-result plumbing.
#
# PASS iff serial shows "FPUTORTURE PASS leaks=0" and no worker crashed.
set -u
IMG=disk/image64.img
COPY=/tmp/nanos-fpu.img
SER=/tmp/nanos-fpu.log
MON=/tmp/nanos-fpu-qmon.sock
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }
cp "$IMG" "$COPY"; rm -f "$SER" "$MON"
pkill -9 -f "qemu-system-x86_64.*$COPY" 2>/dev/null; sleep 1

qemu-system-x86_64 -accel tcg,thread=multi -cpu qemu64 -smp 4 -m 512 \
    -drive file="$COPY",format=raw \
    -display none -serial file:"$SER" -monitor unix:"$MON",server,nowait -no-reboot &
QPID=$!
cleanup() { kill -9 "$QPID" 2>/dev/null; rm -f "$COPY"; }
trap cleanup EXIT

for i in $(seq 1 70); do grep -aq "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done
grep -aq "nanos login:" "$SER" 2>/dev/null || { echo "FAIL: never reached login"; tail -20 "$SER"; exit 1; }
sleep 3

python3 - "$MON" <<'PY'
import socket,time,sys
MON=sys.argv[1]
def cmd(l):
    s=socket.socket(socket.AF_UNIX)
    try: s.connect(MON)
    except Exception as e: print("monitor connect failed:",e); return
    time.sleep(0.2)
    try: s.settimeout(0.3); s.recv(65536)
    except: pass
    s.sendall((l+"\n").encode()); time.sleep(0.3); s.close()
def type_line(t):
    for c in t:
        cmd("sendkey "+c)
    cmd("sendkey ret")
type_line("jan"); time.sleep(3)
type_line("jan"); time.sleep(5)
type_line("fputorture")
PY

DONE=""
for i in $(seq 1 120); do
	if grep -aqE "FPUTORTURE (PASS|FAIL)" "$SER" 2>/dev/null; then DONE=1; break; fi
	sleep 2
done
[ -n "$DONE" ] || { echo "FAIL: fputorture never finished"; tail -15 "$SER"; exit 1; }
if grep -aq "FPUTORTURE PASS leaks=0" "$SER" && ! grep -aq "killed faulting process" "$SER"; then
	echo "PASS: XMM/MXCSR state survives preemption across 4 workers on 4 CPUs (no FPU context leak)."
	exit 0
fi
echo "FAIL: FPU/SSE context leak detected:"
grep -aE "FPUTORTURE|killed faulting" "$SER" | head -10
exit 1
