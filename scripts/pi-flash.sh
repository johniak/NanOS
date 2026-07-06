#!/usr/bin/env bash
# pi-flash.sh — push the whole image64.img to the pendrak Pi (rsync --inplace, resumable).
# Detaches the LUN so the mass-storage function releases the backing file, rsyncs in place,
# then reattaches (always, via trap). Run only while the Dell is off/rebooting.
set -euo pipefail
PI_HOST="${PI_HOST:-pi@pendrak.local}"
IMG="${IMG:-disk/image64.img}"
PI_IMG=/home/pi/nanos/image64.img
SSH="ssh -o ConnectTimeout=8 -o BatchMode=yes"

reattach(){ $SSH "$PI_HOST" 'sudo -n /usr/local/bin/nanos-lun.sh attach' >/dev/null 2>&1 || true; }

[ -f "$IMG" ] || { echo "missing $IMG — run 'make image64' first" >&2; exit 1; }
$SSH "$PI_HOST" 'mkdir -p ~/nanos' \
  || { echo "cannot reach $PI_HOST" >&2; exit 1; }
$SSH "$PI_HOST" 'command -v rsync >/dev/null' \
  || { echo "rsync missing on $PI_HOST — install with: apt-get install -y rsync" >&2; exit 1; }

echo ">> detaching LUN"
$SSH "$PI_HOST" 'sudo -n /usr/local/bin/nanos-lun.sh detach'
trap reattach EXIT                      # never leave the Dell diskless, even on rsync failure

echo ">> rsync $IMG -> $PI_HOST:$PI_IMG"
# macOS ships an old rsync (no --info=progress2); --progress works everywhere.
rsync --inplace --partial -z --progress -e "$SSH" "$IMG" "$PI_HOST:$PI_IMG"

echo ">> reattaching LUN"
reattach
trap - EXIT
echo "full image pushed to $PI_HOST. Boot the Dell."
