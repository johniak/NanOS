#!/usr/bin/env bash
# glpix-scanout-test — the render→scanout proof on the virgl QEMU fork.
#
# Two KMS SETCRTC oracles run back to back, each screendumped while it holds the scanout (sleep 3):
#   1. drmtest — a 2D dumb buffer filled blue (0xFF2060C0). Validates that `screendump` can capture
#      the gl=es scanout AT ALL (the advisor's caution: an earlier black screendump was never
#      confirmed as a working capture oracle on this display). Expect a blue-dominant frame.
#   2. glpix — a virgl 3D resource CLEARED to magenta by the host GPU, then SETCRTC'd. This is the
#      decisive test: a GPU-RENDERED resource resolved to the display host-side, no CPU readback.
#      Expect a magenta-dominant frame.
#
# Decision matrix:
#   blue OK + magenta OK  → render→scanout works; the glReadPixels-from-FBO quirk is off the
#                           desktop critical path. Proceed to the nwm GL backend with confidence.
#   blue OK + magenta NO  → a real bug on the actual display path; minimal reproducer in hand.
#   blue NO               → screendump can't capture gl=es; fall back to the cocoa window (look).
#
# Env: QEMU_GL, IMG override the fork binary / disk image. Serial markers + PPM color stats print here.
set -u
cd "$(dirname "$0")/.."
IMG=${IMG:-disk/image64.img}
QEMU_GL=${QEMU_GL:-$HOME/Projects/nanos-sdk-work/qemu-virgl-kosmickrisp/bin/qemu-system-x86_64}
SER=$(mktemp -t nanos-glpix.XXXXXX.log)
MON=$(mktemp -t nanos-glpix.XXXXXX.mon); rm -f "$MON"
DUMP_DRM=$(mktemp -t nanos-glpix-drm.XXXXXX.ppm)
DUMP_GL=$(mktemp -t nanos-glpix-gl.XXXXXX.ppm)

[ -x "$QEMU_GL" ] || { echo "glpix-test: no virgl QEMU at $QEMU_GL (set QEMU_GL=...)"; exit 1; }
[ -f "$IMG" ]     || { echo "glpix-test: no image at $IMG (run 'make image64' first)"; exit 1; }

# Ad-hoc re-sign the tap dylibs the fork dlopen()s (bottle relocation invalidates their signatures).
if command -v codesign >/dev/null 2>&1 && command -v brew >/dev/null 2>&1; then
  for keg in libepoxy angle virglrenderer; do
    d=$(brew --prefix "startergo/$keg/$keg" 2>/dev/null)/lib
    [ -d "$d" ] || continue
    for lib in "$d"/*.dylib; do [ -f "$lib" ] || continue
      codesign -v "$lib" >/dev/null 2>&1 || codesign --force --sign - "$lib" >/dev/null 2>&1; done
  done
fi

pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null; sleep 1
"$QEMU_GL" -cpu qemu64 -accel tcg,thread=multi -smp 1 -m 512 \
    -drive file="$IMG",format=raw -device virtio-gpu-gl-pci -display cocoa,gl=es \
    -serial file:"$SER" -monitor unix:"$MON",server,nowait -no-reboot >/dev/null 2>&1 &
QPID=$!
trap 'kill -9 "$QPID" 2>/dev/null; rm -f "$MON"' EXIT
echo "glpix-test: serial log at $SER"

echo "glpix-test: booting on the virgl fork (TCG, single vCPU — ~15s)..."
for i in $(seq 1 150); do grep -q "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done
grep -q "nanos login:" "$SER" 2>/dev/null || { echo "FAIL: never reached login"; tail -20 "$SER"; exit 1; }

MON="$MON" DUMP_DRM="$DUMP_DRM" DUMP_GL="$DUMP_GL" python3 - <<'PY'
import socket,time,os
MON=os.environ["MON"]; s=None
for _ in range(30):
    try: s=socket.socket(socket.AF_UNIX); s.connect(MON); break
    except Exception: time.sleep(0.3); s=None
if s is None: print("FATAL: could not open monitor"); raise SystemExit(1)
time.sleep(0.3)
def cmd(c): s.sendall((c+"\n").encode()); time.sleep(0.1)
def key(k): cmd("sendkey "+k); time.sleep(0.06)
def typ(w):
    m={'.':'dot','/':'slash','-':'minus','_':'shift-minus'}
    for c in w: key(m.get(c,c)); time.sleep(0.03)
    key("ret")
key("ctrl-alt-f1"); time.sleep(1.5)
typ("root");  time.sleep(1.5)
typ("nanos"); time.sleep(2.0)
# 1) drmtest: 2D blue SETCRTC, holds scanout for sleep(3) — screendump mid-hold.
typ("drmtest"); time.sleep(2.0)
cmd("screendump "+os.environ["DUMP_DRM"]); time.sleep(1.5)
time.sleep(3.0)   # let drmtest finish (virgl_part) + return to the shell
# 2) glpix: virgl 3D magenta render → SETCRTC, holds scanout for sleep(3) — screendump mid-hold.
typ("glpix"); time.sleep(2.0)
cmd("screendump "+os.environ["DUMP_GL"]); time.sleep(1.5)
time.sleep(2.0)
s.close()
PY

echo "===== serial markers ====="
grep -E "drmtest:|glpix:" "$SER" 2>/dev/null || echo "(none)"
echo "=========================="

# PPM color analysis: report the dominant color + counts of blue (drmtest) / magenta (glpix).
analyze() {
  local ppm="$1" want="$2"
  python3 - "$ppm" "$want" <<'PY'
import sys
path,want=sys.argv[1],sys.argv[2]
try:
    d=open(path,'rb').read()
except Exception as e:
    print(f"  {want}: no screendump ({e})"); raise SystemExit
if not d.startswith(b'P6'): print(f"  {want}: not a P6 PPM"); raise SystemExit
# parse header: P6 <w> <h> <maxval> then binary
i=2; vals=[]
while len(vals)<3:
    while i<len(d) and d[i:i+1].isspace(): i+=1
    if d[i:i+1]==b'#':
        while i<len(d) and d[i:i+1]!=b'\n': i+=1
        continue
    j=i
    while j<len(d) and not d[j:j+1].isspace(): j+=1
    vals.append(int(d[i:j])); i=j
w,h,mx=vals; i+=1
px=d[i:i+w*h*3]
from collections import Counter
cnt=Counter(); blue=magenta=black=0; N=w*h
for k in range(0,len(px)-2,3):
    r,g,b=px[k],px[k+1],px[k+2]
    cnt[(r//64,g//64,b//64)]+=1
    if r<80 and g<80 and b<80: black+=1
    if b>150 and r<120 and g<120: blue+=1
    if r>150 and b>150 and g<120: magenta+=1
top=cnt.most_common(3)
print(f"  {want}: {w}x{h}  black={100*black//max(N,1)}%  blue={100*blue//max(N,1)}%  magenta={100*magenta//max(N,1)}%")
print(f"         top buckets (r,g,b /64): {top}")
PY
}
echo "===== screendump analysis ====="
echo "drmtest (expect blue-dominant → screendump oracle works on gl=es):"
analyze "$DUMP_DRM" "blue"
echo "glpix (expect magenta-dominant → GPU render reached the display):"
analyze "$DUMP_GL" "magenta"
echo "==============================="
echo "PPMs kept for inspection:"
echo "  drmtest: $DUMP_DRM"
echo "  glpix:   $DUMP_GL"
# Don't delete the PPMs (trap only removes SER/MON) so they can be eyeballed / sent to the user.
