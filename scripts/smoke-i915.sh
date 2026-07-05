#!/usr/bin/env bash
# smoke-i915.sh — the i915 link + load + bring-up-harness gate (Dell GPU plan, Task 5/6).
#
# The full UNMODIFIED Linux 6.12 i915 DRM driver is linked into bin/i915.nkext (276 objects +
# full TTM + DP/display helpers + shared DRM core + LinuxKPI shim + kext/i915 glue) and ships in
# the default image. It is gated by the arm-knob /nanos/config/i915:
#
#   Part A (default image, UNARMED): i915 must be a safe no-op — nkext_init returns before any
#     mem_map / DRM core / i915_init, so a normal boot is unaffected (this is what lets it co-exist
#     with virtio_gpu with no DRM-core double-init). Assert "i915: not armed" + clean boot.
#
#   Part B (throwaway image, ARMED via /nanos/config/i915=1): the 3 MB kext must load, relocate,
#     run drm_core_init + the unmodified i915_init() (all module subfuncs), turn DRM debug to 0x1ff,
#     narrate every stage, and — no Intel GPU on QEMU — reach the idle path with zero faults. AND the
#     bring-up markers must PERSIST to /nanos/logs/i915-boot.txt on the writable root (the Dell log
#     channel that survives a hang) — verified by reading the file back out of the image after boot.
set -u
SRC=disk/image64.img
ARMIMG=disk/i915arm-smoke.img
KNOB=bin/i915-arm-knob            # repo-local "1" file for debugfs (container sees /src)
PART=69206016
SERA=/tmp/nanos-i915-unarmed.log
SERB=/tmp/nanos-i915-armed.log
LOGDUMP=/tmp/nanos-i915-bootlog.txt
rm -f "$SERA" "$SERB" "$LOGDUMP"
[ -f "$SRC" ] || { echo "FAIL: $SRC missing — run 'make image64' first"; exit 2; }

QPID=""
# Guard the kill: an empty QPID must NOT become `kill -9 0` (that signals the whole process group,
# incl. the parent make — which would report failure even after a PASS).
cleanup() { [ -n "${QPID:-}" ] && kill -9 "$QPID" 2>/dev/null; rm -f "$ARMIMG" "$KNOB"; return 0; }
trap cleanup EXIT

# "reached userspace" = the login greeter OR the serial root shell — either proves boot completed.
# Which prompt lands on the serial console varies with display/greeter timing; both are success.
READY='nanos login:|nsh\$'
boot() {  # $1=image $2=serial-log
	pkill -9 -f "qemu-system-x86_64.*$1" 2>/dev/null; sleep 1
	qemu-system-x86_64 -accel tcg,thread=multi -cpu qemu64 -smp 1 -m 512 -drive file="$1",format=raw \
	    -display none -serial file:"$2" -no-reboot &
	QPID=$!
	for i in $(seq 1 60); do grep -qE "$READY" "$2" 2>/dev/null && break; sleep 1; done
	kill -9 "$QPID" 2>/dev/null; QPID=""
	grep -qE "$READY" "$2" 2>/dev/null
}

fault() { grep -iE "panic|#PF|GPF|unhandled|kernel fault|assertion failed" "$1" 2>/dev/null; }

# ---- Part A: default image, i915 UNARMED = safe no-op --------------------------------------
echo "smoke-i915: Part A — default image, i915 unarmed"
if ! boot "$SRC" "$SERA"; then
	echo "FAIL(A): default image never reached a userspace prompt (unarmed i915 broke boot?)"; tail -25 "$SERA"; exit 1
fi
if ! grep -q "i915: not armed" "$SERA"; then
	echo "FAIL(A): i915 did not report the disarmed no-op path"; grep -i i915 "$SERA" | tail; exit 1
fi
if fault "$SERA"; then echo "FAIL(A): fault on the default (unarmed) boot"; fault "$SERA" | tail; exit 1; fi
echo "  PASS(A): i915 is a safe no-op in the default image."

# ---- Part B: throwaway image, i915 ARMED ---------------------------------------------------
echo "smoke-i915: Part B — armed bring-up + persistent log"
cp "$SRC" "$ARMIMG"
printf '1' > "$KNOB"
docker run --rm -v "$PWD":/src -w /src nanos-build bash -c \
  "printf 'rm /nanos/config/i915\nwrite $KNOB /nanos/config/i915\n' | debugfs -w '$ARMIMG?offset=$PART' >/dev/null 2>&1" \
  || { echo "FAIL(B): could not arm /nanos/config/i915"; exit 1; }

if ! boot "$ARMIMG" "$SERB"; then
	echo "FAIL(B): armed image never reached a userspace prompt (i915 bring-up hang?)"; grep -i i915 "$SERB" | tail -25; exit 1
fi
for marker in \
	"bring-up session armed" \
	"mem_map init OK" \
	"DRM core init OK" \
	"unmodified Linux 6.12 i915 driver registered" \
	"no Intel GPU present"; do
	if ! grep -q "$marker" "$SERB"; then
		echo "FAIL(B): missing bring-up marker: '$marker'"; grep -i i915 "$SERB" | tail -20; exit 1
	fi
done
if fault "$SERB"; then echo "FAIL(B): fault during armed i915 bring-up"; fault "$SERB" | tail; exit 1; fi

# The markers must ALSO have persisted to the on-disk log (the Dell hang-survival channel).
docker run --rm -v "$PWD":/src -w /src nanos-build bash -c \
  "debugfs -R 'cat /nanos/logs/i915-boot.txt' '$ARMIMG?offset=$PART' 2>/dev/null" > "$LOGDUMP"
if ! grep -q "bring-up session armed" "$LOGDUMP" || ! grep -q "no Intel GPU present" "$LOGDUMP"; then
	echo "FAIL(B): bring-up markers did not persist to /nanos/logs/i915-boot.txt (knx_file_append)"; echo "--- dump ---"; cat "$LOGDUMP"; exit 1
fi
echo "  PASS(B): armed i915 loads, narrates every stage, idles clean, and the log persisted to disk."

echo "PASS: i915 links + loads; unarmed no-op in the default image, armed bring-up narrates to screen AND /nanos/logs/i915-boot.txt."
