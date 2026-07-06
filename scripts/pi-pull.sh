#!/usr/bin/env bash
# pi-pull.sh — retrieve NanOS-modified data from the pendrak backing image.
#   MODE=files (default): extract PATHS from the image on the Pi, scp them into DEST dir.
#   MODE=image          : rsync the whole image64.img from the Pi into the DEST file.
# Run ONLY while the Dell is OFF (writes flushed; avoids two-writer corruption).
set -euo pipefail
PI_HOST="${PI_HOST:-pi@pendrak.local}"
MODE="${MODE:-files}"
SSH="ssh -o ConnectTimeout=8 -o BatchMode=yes"

if [ "$MODE" = image ]; then
  DEST="${DEST:-disk/image64-dell.img}"
  echo ">> rsync $PI_HOST:image64.img -> $DEST  (the Dell must be OFF)"
  rsync --inplace --partial -z --progress -e "$SSH" "$PI_HOST:/home/pi/nanos/image64.img" "$DEST"
  echo "pulled whole image -> $DEST"
else
  PATHS="${PATHS:-/nanos/logs}"
  DEST="${DEST:-pull-dell}"
  echo ">> extract [$PATHS] on $PI_HOST (the Dell must be OFF)"
  $SSH "$PI_HOST" "sudo -n /usr/local/bin/nanos-pull.sh $PATHS"
  mkdir -p "$DEST"
  scp -o ConnectTimeout=8 -r "$PI_HOST:nanos/pull/." "$DEST/"
  echo "pulled [$PATHS] -> $DEST/"
fi
