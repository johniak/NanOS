#!/usr/bin/env bash
#
# i915-log.sh — dump the persisted i915 boot log from the Kingston stick after a Dell boot.
#
# The armed harness tees its markers to /disks/main/nanos/logs/i915-boot.txt (see
# kext/i915/i915_entry.c), which survives the reboot. This reads that file straight off the
# stick's ext partition with host debugfs — bring the stick back to the Mac and run it.
#
# Env: USB_NAME  media-name match (regex)  (default "Kingston|DataTraveler")
# Args: optional path to also save a copy of the log.
# macOS only; needs sudo for the raw partition.
set -euo pipefail

USB_NAME="${USB_NAME:-Kingston|DataTraveler}"
SAVE="${1:-}"

die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
note() { printf '\033[36m%s\033[0m\n' "$*"; }

DBG=""
for c in debugfs /opt/homebrew/opt/e2fsprogs/sbin/debugfs /usr/local/opt/e2fsprogs/sbin/debugfs; do
    command -v "$c" >/dev/null 2>&1 && { DBG="$c"; break; }
done
[ -n "$DBG" ] || die "debugfs not found — 'brew install e2fsprogs'"

DEV=""
for d in $(diskutil list external physical 2>/dev/null \
           | awk '/^\/dev\/disk[0-9]+ \(external, physical\)/ {print $1}'); do
    name=$(diskutil info "$d" 2>/dev/null | awk -F': *' '/Device \/ Media Name/ {print $2}')
    printf '%s' "$name" | grep -Eiq "$USB_NAME" && { [ -n "$DEV" ] && die "multiple USB matches"; DEV="$d"; NAME="$name"; }
done
[ -n "$DEV" ] || die "no USB disk matching '$USB_NAME' — is the Kingston plugged in?"
PART=$(diskutil list "$DEV" | awk '/Linux Filesystem/ {print $NF}' | tail -1)
[ -n "$PART" ] || die "no Linux Filesystem partition on $DEV"
PDEV="/dev/$PART"

diskutil unmountDisk "$DEV" >/dev/null 2>&1 || true
# Cache sudo credentials up front with a VISIBLE prompt — the capture below suppresses stderr
# (to strip the debugfs banner), which would otherwise swallow sudo's password prompt and yield
# a spuriously empty result.
sudo -v || die "sudo required to read the raw partition"
note "===== /nanos/logs/i915-boot.txt  ($DEV, $NAME) ====="
out=$(sudo "$DBG" -R "cat /nanos/logs/i915-boot.txt" "$PDEV" 2>/dev/null || true)
if [ -z "$out" ]; then
    die "log empty/absent — harness may not have armed (check knob) or the driver never reached the tee"
fi
printf '%s\n' "$out"
# The userland oracle tees its markers here too (i915test.c) — no console photos needed.
note "===== /nanos/logs/i915test.txt  (i915test tee) ====="
out2=$(sudo "$DBG" -R "cat /nanos/logs/i915test.txt" "$PDEV" 2>/dev/null || true)
if [ -n "$out2" ]; then
    printf '%s\n' "$out2"
else
    note "(absent — i915test was not run this boot, or /nanos/logs was not writable)"
fi
# The GL oracles (gles2info + glkms) tee here (truncated per boot by init's auto-run).
note "===== /nanos/logs/gltest.txt  (gles2info + glkms tee) ====="
out3=$(sudo "$DBG" -R "cat /nanos/logs/gltest.txt" "$PDEV" 2>/dev/null || true)
if [ -n "$out3" ]; then
    printf '%s\n' "$out3"
else
    note "(absent — the GL oracles were not run this boot)"
fi
# The GL compositor's own diagnostics (nwm.c redirects stdout/stderr here on the GL build):
# glkms stage/swap failures with errno, frame telemetry, Mesa loader/driver stderr.
note "===== /nanos/logs/nwm.txt  (nwm-gl stdout/stderr) ====="
out4=$(sudo "$DBG" -R "cat /nanos/logs/nwm.txt" "$PDEV" 2>/dev/null || true)
if [ -n "$out4" ]; then
    printf '%s\n' "$out4"
else
    note "(absent — nwm-gl never started, or a non-GL nwm build)"
fi
if [ -n "$SAVE" ]; then
    { printf '%s\n' "$out"; printf '===== i915test tee =====\n%s\n' "$out2"; printf '===== gltest tee =====\n%s\n' "$out3"; printf '===== nwm tee =====\n%s\n' "$out4"; } > "$SAVE"
    note "saved -> $SAVE"
fi
