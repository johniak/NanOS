#!/usr/bin/env bash
# pi-bootstrap.sh — one-time, idempotent setup of the pendrak Pi from a fresh Raspbian.
# Puts the OTG port in dwc2 peripheral mode, installs the gadget/apply/lun/pull helpers,
# the sudoers drop-in, and the systemd unit. Safe to re-run. Needs one manual reboot after
# the first run (prints the reminder). Requires passwordless sudo for 'pi' already in place.
#
# NOTE on config.txt: the dwc2 overlay MUST live under [all] — on a Pi Zero W a line under a
# [cm4]/[cm5]/[pi5] filter section is silently ignored (this bit us during bring-up), and
# otg_mode=1 keeps the host-only dwc_otg driver. So we comment any mis-sectioned dwc2/otg_mode
# lines and append a clean [all] block.
set -euo pipefail
PI_HOST="${PI_HOST:-pi@pendrak.local}"
SP="$(cd "$(dirname "$0")" && pwd)/pi"
SSH="ssh -o ConnectTimeout=8 -o BatchMode=yes"

$SSH "$PI_HOST" true || { echo "cannot reach $PI_HOST" >&2; exit 1; }
$SSH "$PI_HOST" 'sudo -n true' 2>/dev/null \
  || { echo "passwordless sudo not set up for 'pi' on $PI_HOST — enable it first" >&2; exit 1; }

echo ">> config.txt: dwc2 peripheral under [all] (idempotent)"
$SSH "$PI_HOST" 'set -e; f=/boot/firmware/config.txt
  [ -f "$f".nanos.bak ] || sudo -n cp "$f" "$f".nanos.bak
  # All config.txt edits are marker-guarded so a re-run is a no-op (and never comments our own line).
  if ! grep -q "nanos pendrak gadget (managed)" "$f"; then
    # neutralise any ACTIVE (uncommented) dwc2 overlay or otg_mode lines — they may be mis-sectioned
    sudo -n sed -i "s/^dtoverlay=dwc2/#dtoverlay=dwc2  # (nanos: superseded by [all] block)/" "$f"
    sudo -n sed -i "s/^otg_mode=1/#otg_mode=1  # (nanos: keep dwc2, not dwc_otg)/" "$f"
    printf "\n# --- nanos pendrak gadget (managed) ---\n[all]\ndtoverlay=dwc2,dr_mode=peripheral\n" | sudo -n tee -a "$f" >/dev/null
  fi
  grep -nE "^\[|dtoverlay=dwc2|otg_mode|nanos pendrak" "$f"'

echo ">> cmdline.txt: modules-load=dwc2 (idempotent)"
$SSH "$PI_HOST" 'f=/boot/firmware/cmdline.txt; grep -q "modules-load=dwc2" "$f" || sudo -n sed -i "s/\$/ modules-load=dwc2/" "$f"; cat "$f"'

echo ">> staging dir + placeholder image (if none yet)"
$SSH "$PI_HOST" 'mkdir -p ~/nanos/staging && { [ -f ~/nanos/image64.img ] || truncate -s 320M ~/nanos/image64.img; }'

echo ">> install helpers"
for f in nanos-gadget.sh nanos-lun.sh nanos-apply.sh nanos-pull.sh; do
  scp -o ConnectTimeout=8 "$SP/$f" "$PI_HOST:/tmp/$f"
  $SSH "$PI_HOST" "sudo -n install -m0755 /tmp/$f /usr/local/bin/$f"
done

echo ">> install sudoers (validated)"
scp -o ConnectTimeout=8 "$SP/nanos-pi.sudoers" "$PI_HOST:/tmp/nanos-pi.sudoers"
$SSH "$PI_HOST" 'sudo -n install -m0440 /tmp/nanos-pi.sudoers /etc/sudoers.d/nanos && sudo -n visudo -c >/dev/null && echo "  sudoers OK"'

echo ">> install + enable systemd unit"
scp -o ConnectTimeout=8 "$SP/nanos-gadget.service" "$PI_HOST:/tmp/nanos-gadget.service"
$SSH "$PI_HOST" 'sudo -n install -m0644 /tmp/nanos-gadget.service /etc/systemd/system/nanos-gadget.service && \
  sudo -n systemctl daemon-reload && sudo -n systemctl enable nanos-gadget.service >/dev/null && echo "  service enabled"'

echo ""
echo "bootstrap done. If config.txt/cmdline changed, REBOOT the Pi:  $SSH $PI_HOST sudo -n reboot"
echo "Then push an image:  make flash-dell-pi"
