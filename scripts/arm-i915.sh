#!/usr/bin/env bash
#
# arm-i915.sh — set the i915 arm-knob on the Kingston boot stick WITHOUT a rebuild/reflash.
#
# The i915 kext ships in the default KEXTS but is a no-op ("i915: not armed") unless
# /disks/main/nanos/config/i915 begins with '1' (see kext/i915/i915_entry.c). /disks/main is
# the stick's ext root, so inside the filesystem the knob is /nanos/config/i915. This script
# writes it straight onto the ext partition with host debugfs — no QEMU, no image rebuild.
#
# Env:
#   VALUE     '1' to arm, '0' to disarm     (default 1)
#   USB_NAME  media-name match (regex)      (default "Kingston|DataTraveler")
#
# macOS only (host debugfs from homebrew e2fsprogs); needs sudo for the raw partition.
set -euo pipefail

VALUE="${VALUE:-1}"
USB_NAME="${USB_NAME:-Kingston|DataTraveler}"

die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
note() { printf '\033[36m%s\033[0m\n' "$*"; }

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

# The ext root is the "Linux Filesystem" partition on that disk.
PART=$(diskutil list "$DEV" | awk '/Linux Filesystem/ {print $NF}' | tail -1)
[ -n "$PART" ] || die "no Linux Filesystem partition on $DEV"
PDEV="/dev/$PART"

note "stick     : $DEV  ($NAME)"
note "ext part  : $PDEV"
note "knob      : /nanos/config/i915  <-  '$VALUE'   ($([ "$VALUE" = 1 ] && echo ARM || echo disarm))"

diskutil unmountDisk "$DEV" >/dev/null 2>&1 || true
# Cache sudo up front with a visible prompt (the debugfs calls below suppress stderr, which would
# otherwise eat the password prompt and fail silently).
sudo -v || die "sudo required to write the raw partition"

TMP=$(mktemp); printf '%s' "$VALUE" > "$TMP"
CMDS=$(mktemp)
cat > "$CMDS" <<EOF
mkdir /nanos
mkdir /nanos/config
rm /nanos/config/i915
write $TMP /nanos/config/i915
EOF
# debugfs -f runs the script; mkdir/rm on an existing/absent path just print a warning and continue.
sudo "$DBG" -w -f "$CMDS" "$PDEV" >/dev/null 2>&1 || true
rm -f "$CMDS" "$TMP"

got=$(sudo "$DBG" -R "cat /nanos/config/i915" "$PDEV" 2>/dev/null | tr -d '\0')
[ "$got" = "$VALUE" ] || die "verify failed: knob reads '$got', expected '$VALUE'"
note "ok — knob = '$got'. Eject-safe; boot the Dell."
diskutil eject "$DEV" >/dev/null 2>&1 || true
