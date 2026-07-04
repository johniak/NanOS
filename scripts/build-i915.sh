#!/usr/bin/env bash
#
# build-i915.sh — compile-campaign scorer for the vendored (unmodified) Linux 6.12 i915 driver.
#
# Compiles every object in scripts/i915-objs.txt against the LinuxKPI shim and reports clean/total,
# with the failing files grouped by their first compiler error (the campaign's cluster map). This is
# the score keeper for the link-error-driven lift: fix by (a) vendoring a real include/linux/*.h,
# (b) a real KPI impl in linuxkpi/, or (c) a force-include — NEVER by editing external/linux-6.12/**.
#
# The object list is scripts/i915-objs.txt, derived from drivers/gpu/drm/i915/Makefile (v6.12) with
# these CONFIG choices: DRM_I915=y, X86=y; COMPAT/DEBUG_FS/PERF_EVENTS/HWMON/ACPI/FBDEV_EMULATION/
# DP_TUNNEL/PXP/CAPTURE_ERROR/SELFTEST/GVT/WERROR = n. (PXP/GuC/HuC/GSC *base* code is unconditional
# in i915-y and is compiled; only the CONFIG-gated extras are dropped.)
#
# Env (set by the Makefile `i915-probe` target so flags stay DRY with the kext build):
#   CC     - the cross compiler (default x86_64-elf-gcc)
#   CFLAGS - the full LinuxKPI + vendored-include + -I<i915> -DI915 flag set
set -u

SRC=external/linux-6.12/drivers/gpu/drm/i915
LIST=scripts/i915-objs.txt
OUT=${I915_BUILD_OUT:-/tmp/i915-build}
: "${CC:=x86_64-elf-gcc}"
: "${CFLAGS:=}"

mkdir -p "$OUT"
: > "$OUT/errors.log"; : > "$OUT/clean.txt"; : > "$OUT/fail.txt"
total=0; clean=0; fail=0

while IFS= read -r o; do
	[ -z "$o" ] && continue
	case "$o" in \#*) continue ;; esac
	total=$((total + 1))
	flat=$(printf '%s' "$o" | tr '/' '_')
	if $CC $CFLAGS -c "$SRC/$o.c" -o "$OUT/$flat.o" 2> "$OUT/$flat.err"; then
		clean=$((clean + 1)); printf '%s\n' "$o" >> "$OUT/clean.txt"
	else
		fail=$((fail + 1)); printf '%s\n' "$o" >> "$OUT/fail.txt"
		first=$(grep -m1 -E "error:|fatal error:" "$OUT/$flat.err" | head -1)
		printf '%s :: %s\n' "$o" "$first" >> "$OUT/errors.log"
	fi
done < "$LIST"

echo "================ i915 compile campaign ================"
echo "clean/total: $clean/$total   (fail: $fail)"
echo "------- first-error clusters (top 30, normalised) -------"
# Normalise line numbers / quoted identifiers so distinct sites of the same missing symbol group.
sed 's/^[^:]*:: //' "$OUT/errors.log" \
	| sed -E "s/[0-9]+/N/g; s/'[^']*'/X/g" \
	| sort | uniq -c | sort -rn | head -30
echo "-------------------------------------------------------"
echo "clean list: $OUT/clean.txt   failing list: $OUT/fail.txt   full errors: $OUT/errors.log"
[ "$fail" -eq 0 ]
