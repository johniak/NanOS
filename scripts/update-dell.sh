#!/usr/bin/env bash
#
# update-dell.sh — push ONLY the changed kernel + i915 kext onto the Kingston boot stick,
# skipping the full ~320 MiB image dd. The kernel (/nanos/core/kernel.bin) and the driver
# (/nanos/kext/i915.nkext) both live on the ext root partition (P3) — the same partition the
# image build itself populates with host debugfs (Makefile image64) and the same one arm-i915.sh
# writes the knob to. So an i915 bring-up iteration only needs those two files rewritten (~6 MiB)
# plus a re-arm, not a whole-disk reflash.
#
# This is the fast inner loop for the Dell i915 campaign: `make update-dell` (rebuilds kernel.bin
# + i915.nkext, then runs this) -> boot Dell -> `make i915-log`.
#
# Env:
#   KERNEL    kernel binary to push       (default bin/k64/kernel.bin)
#   KEXT      i915 kext to push           (default bin/i915.nkext)
#   ARM       '1' to (re)arm i915, '0' no (default 1)
#   USB_NAME  media-name match (regex)    (default "Kingston|DataTraveler")
#   NO_EJECT  =1 to leave the stick attached afterwards
#
# macOS only (host debugfs from homebrew e2fsprogs); needs sudo for the raw partition.
set -euo pipefail

KERNEL="${KERNEL:-bin/k64/kernel.bin}"
KEXT="${KEXT:-bin/i915.nkext}"
ARM="${ARM:-1}"
USB_NAME="${USB_NAME:-Kingston|DataTraveler}"

die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
note() { printf '\033[36m%s\033[0m\n' "$*"; }

[ -f "$KERNEL" ] || die "kernel not found: $KERNEL (run 'make image64' first)"
[ -f "$KEXT" ]   || die "i915 kext not found: $KEXT (run 'make image64' first)"

DBG=""
for c in debugfs /opt/homebrew/opt/e2fsprogs/sbin/debugfs /usr/local/opt/e2fsprogs/sbin/debugfs; do
    command -v "$c" >/dev/null 2>&1 && { DBG="$c"; break; }
done
[ -n "$DBG" ] || die "debugfs not found — 'brew install e2fsprogs'"

# Locate the Kingston stick (external removable disk whose media name matches USB_NAME).
DEV=""
for d in $(diskutil list external physical 2>/dev/null \
           | awk '/^\/dev\/disk[0-9]+ \(external, physical\)/ {print $1}'); do
    name=$(diskutil info "$d" 2>/dev/null | awk -F': *' '/Device \/ Media Name/ {print $2}')
    printf '%s' "$name" | grep -Eiq "$USB_NAME" && { [ -n "$DEV" ] && die "multiple USB matches — unplug extras"; DEV="$d"; NAME="$name"; }
done
[ -n "$DEV" ] || die "no USB disk matching '$USB_NAME' — is the Kingston plugged in?"

# The ext root (kernel.bin + kexts + config) is the "Linux Filesystem" partition on that disk.
PART=$(diskutil list "$DEV" | awk '/Linux Filesystem/ {print $NF}' | tail -1)
[ -n "$PART" ] || die "no Linux Filesystem partition on $DEV"
PDEV="/dev/$PART"

note "stick     : $DEV  ($NAME)"
note "ext part  : $PDEV"
note "kernel    : $KERNEL  ($(wc -c < "$KERNEL" | tr -d ' ') B)  -> /nanos/core/kernel.bin"
note "i915 kext : $KEXT  ($(wc -c < "$KEXT" | tr -d ' ') B)  -> /nanos/kext/i915.nkext"
[ "$ARM" = 1 ] && note "arm       : /nanos/config/i915 <- '1'"

diskutil unmountDisk "$DEV" >/dev/null 2>&1 || true
# Cache sudo up front with a visible prompt (the debugfs calls suppress stderr, which would
# otherwise eat the password prompt and fail silently).
sudo -v || die "sudo required to write the raw partition"

# One debugfs script: rm+write each file (the exact idiom the image build uses), plus the arm knob.
ARMTMP=""
CMDS=$(mktemp)
{
    echo "rm /nanos/core/kernel.bin"
    echo "write $KERNEL /nanos/core/kernel.bin"
    echo "rm /nanos/kext/i915.nkext"
    echo "write $KEXT /nanos/kext/i915.nkext"
    if [ "$ARM" = 1 ]; then
        ARMTMP=$(mktemp); printf '1' > "$ARMTMP"
        echo "mkdir /nanos/config"
        echo "rm /nanos/config/i915"
        echo "write $ARMTMP /nanos/config/i915"
    fi
} > "$CMDS"
sudo "$DBG" -w -f "$CMDS" "$PDEV" >/dev/null 2>&1 || true
rm -f "$CMDS"; [ -n "$ARMTMP" ] && rm -f "$ARMTMP"

# Verify: the on-stick sizes must match the host artifacts (a short write = a truncated kernel that
# would triple-fault the box), and the arm knob must read '1'.
check_size() {   # $1 = ext path, $2 = host file
    local want got
    want=$(wc -c < "$2" | tr -d ' ')
    got=$(sudo "$DBG" -R "stat $1" "$PDEV" 2>/dev/null | grep -oE 'Size: [0-9]+' | head -1 | grep -oE '[0-9]+')
    [ "$got" = "$want" ] || die "verify FAILED for $1 — on-stick size '$got' != host '$want' (short write)"
    note "  ok $1  ($got B)"
}
check_size /nanos/core/kernel.bin "$KERNEL"
check_size /nanos/kext/i915.nkext "$KEXT"
if [ "$ARM" = 1 ]; then
    got=$(sudo "$DBG" -R "cat /nanos/config/i915" "$PDEV" 2>/dev/null | tr -d '\0')
    [ "$got" = 1 ] || die "arm verify FAILED: knob reads '$got'"
    note "  ok /nanos/config/i915 = '1'"
fi

note "updated — kernel + i915 pushed, no full reflash. Boot the Dell, then 'make i915-log'."
[ "${NO_EJECT:-}" = 1 ] || diskutil eject "$DEV" >/dev/null 2>&1 || true
