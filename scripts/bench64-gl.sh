#!/usr/bin/env bash
# bench64-gl.sh — automated GUI-rendering benchmark (`make bench64-gl`).
#
# Boots the GL desktop image in the virgl QEMU fork (same boot + VT7 login dance as
# smoke-virtio-gpu-gl.sh), launches nwbench via the compositor's Cmd+R Run dialog, and lets it
# run UNATTENDED in auto mode: before boot, $NWBENCH_SECS (default 120) is seeded into the image
# as /users/jan/.nwbench-auto; nwbench consumes the flag, benchmarks for that long, writes
# /users/jan/nwbench-result.txt (fsync'd), then prints the "nwbench: done" serial marker.
# On the marker the script quits QEMU, dumps the result file out of the image with debugfs and
# prints it; a copy is archived to scratch/bench/<timestamp>.txt + scratch/bench/latest.txt.
#
# Numbers are only comparable BETWEEN RUNS ON THE SAME HOST/ACCEL (single-core TCG here, which
# is interpreter-speed): treat them as A/B for rasterizer/toolkit changes, not absolute targets.
#
# Like the GL smoke, this is a DEVELOPER gate (needs the virgl QEMU fork + a macOS GUI session);
# it SKIPs cleanly when the fork is absent.
set -u
cd "$(dirname "$0")/.."
IMG=disk/image64-gl.img
PART="$IMG?offset=69206016"
QEMU_VIRGL_HOME=${QEMU_VIRGL_HOME:-$HOME/Projects/nanos-sdk-work/qemu-virgl-kosmickrisp}
QEMU_GL=${QEMU_GL:-$QEMU_VIRGL_HOME/bin/qemu-system-x86_64}
SECS=${NWBENCH_SECS:-120}
SER=/tmp/nanos-bench.log
MON=/tmp/nanos-bench.mon
rm -f "$SER" "$MON"

DEBUGFS=$(command -v debugfs || true)
[ -n "$DEBUGFS" ] || DEBUGFS=/opt/homebrew/opt/e2fsprogs/sbin/debugfs
[ -x "$DEBUGFS" ] || { echo "SKIP: no debugfs (brew install e2fsprogs)"; exit 0; }
[ -x "$QEMU_GL" ] || { echo "SKIP: no virgl QEMU at $QEMU_GL (set QEMU_GL)"; exit 0; }
[ -f "$IMG" ]     || { echo "SKIP: no $IMG — run 'make image64-gl' first"; exit 0; }

# seed the auto flag + clear any stale result so we can't read yesterday's numbers
printf '%s' "$SECS" > /tmp/nwbench-auto.seed
printf 'rm /users/jan/.nwbench-auto\nrm /users/jan/nwbench-result.txt\nwrite /tmp/nwbench-auto.seed /users/jan/.nwbench-auto\nset_inode_field /users/jan/.nwbench-auto mode 0100644\n' \
  | "$DEBUGFS" -w "$PART" >/dev/null 2>&1

pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null; sleep 1
"$QEMU_GL" -cpu qemu64 -accel tcg,thread=multi -smp 1 -m 512 \
    -drive file="$IMG",format=raw -device virtio-gpu-gl-pci -vga none \
    -display cocoa,gl=es,zoom-to-fit=on \
    -serial file:"$SER" -monitor unix:"$MON",server,nowait -no-reboot >/dev/null 2>&1 &
QPID=$!
trap 'kill -9 "$QPID" 2>/dev/null; rm -f "$MON"' EXIT
echo "bench64-gl: QEMU pid=$QPID serial=$SER, benchmark ${SECS}s"

for i in $(seq 1 180); do grep -q "tty7 greeter\|nanos login:" "$SER" 2>/dev/null && break; sleep 2; done
grep -q "tty7 greeter\|nanos login:" "$SER" 2>/dev/null || { echo "FAIL: never reached login"; tail -25 "$SER"; exit 1; }
sleep 3

SER="$SER" MON="$MON" SECS="$SECS" python3 - <<'PY'
import socket,time,os
SER=os.environ["SER"]; MON=os.environ["MON"]; SECS=int(os.environ["SECS"]); s=None
for _ in range(30):
    try: s=socket.socket(socket.AF_UNIX); s.connect(MON); break
    except Exception: time.sleep(0.3); s=None
if s is None: print("FATAL: no monitor"); raise SystemExit(1)
time.sleep(0.3)
def cmd(c): s.sendall((c+"\n").encode()); time.sleep(0.12)
def key(k): cmd("sendkey "+k); time.sleep(0.06)
def typ(w):
    for c in w: key(c); time.sleep(0.03)
    key("ret")
def serial():
    try: return open(SER, errors="ignore").read()
    except OSError: return ""
def desktop_up(deadline):
    while time.time() < deadline:
        if "nwm: GL compositor active" in serial(): return True
        time.sleep(2)
    return False
deadline = time.time() + 300
attempt = 0
while time.time() < deadline:
    attempt += 1
    key("ret")
    cmd("sendkey ctrl-alt-f7"); time.sleep(7)
    typ("jan"); time.sleep(4)
    typ("jan"); time.sleep(2)
    if desktop_up(time.time() + 30):
        print("desktop up (attempt %d)" % attempt); break
    print("desktop not up after attempt %d, retrying" % attempt)
else:
    raise SystemExit(1)
time.sleep(8)                        # let the desktop settle
for run_try in range(3):             # open Cmd+R Run, launch nwbench
    cmd("sendkey meta_l-r"); time.sleep(2)
    typ("nwbench"); time.sleep(6)
    if "nwbench:" in serial():
        break
    print("run-dialog attempt %d: no nwbench output yet" % (run_try+1))
if "nwbench:" not in serial():
    print("FATAL: nwbench never started"); raise SystemExit(1)
print("nwbench running, waiting up to %ds for the report" % (SECS+120))
stop = time.time() + SECS + 120
while time.time() < stop and "nwbench: done" not in serial():
    time.sleep(5)
if "nwbench: done" not in serial():
    print("FATAL: no 'nwbench: done' marker"); raise SystemExit(1)
time.sleep(2)
cmd("quit")                          # clean QEMU exit so the image is settled for debugfs
s.close()
PY
[ $? -eq 0 ] || { echo "FAIL: benchmark run did not complete"; tail -15 "$SER"; exit 1; }
wait "$QPID" 2>/dev/null

mkdir -p scratch/bench
STAMP=$(date +%Y%m%d-%H%M%S)
OUT=scratch/bench/$STAMP.txt
"$DEBUGFS" -R "dump /users/jan/nwbench-result.txt $OUT" "$PART" >/dev/null 2>&1
[ -s "$OUT" ] || { echo "FAIL: no /users/jan/nwbench-result.txt in the image"; exit 1; }
cp "$OUT" scratch/bench/latest.txt
echo "bench64-gl: result ($OUT):"
cat "$OUT"
echo "PASS: benchmark completed; latest -> scratch/bench/latest.txt"
