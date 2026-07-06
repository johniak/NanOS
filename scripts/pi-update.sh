#!/usr/bin/env bash
# pi-update.sh — fast i915 loop: push ONLY kernel.bin + i915.nkext to the pendrak Pi and run
# the surgical apply. Run only while the Dell is off/rebooting.
set -euo pipefail
PI_HOST="${PI_HOST:-pi@pendrak.local}"
KERNEL="${KERNEL:-bin/k64/kernel.bin}"
KEXT="${KEXT:-bin/i915.nkext}"
ARM="${ARM:-1}"
SSH="ssh -o ConnectTimeout=8 -o BatchMode=yes"

[ -f "$KERNEL" ] || { echo "missing $KERNEL — run 'make kernel-kext' first" >&2; exit 1; }
[ -f "$KEXT" ]   || { echo "missing $KEXT — run 'make kernel-kext' first" >&2; exit 1; }
$SSH "$PI_HOST" 'mkdir -p ~/nanos/staging' \
  || { echo "cannot reach $PI_HOST" >&2; exit 1; }

scp -o ConnectTimeout=8 -C "$KERNEL" "$PI_HOST:nanos/staging/kernel.bin"
scp -o ConnectTimeout=8 -C "$KEXT"   "$PI_HOST:nanos/staging/i915.nkext"
$SSH "$PI_HOST" "sudo -n /usr/local/bin/nanos-apply.sh $ARM"
echo "update pushed to $PI_HOST — boot the Dell, then 'make i915-log-pi'."
