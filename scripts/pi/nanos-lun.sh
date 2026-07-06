#!/usr/bin/env bash
# nanos-lun.sh — detach/attach the mass-storage LUN backing file (fixed command for sudoers).
set -euo pipefail
LUN=/sys/kernel/config/usb_gadget/nanos/functions/mass_storage.0/lun.0
IMG=/home/pi/nanos/image64.img
case "${1:-}" in
  detach) echo "" > "$LUN/file" ;;
  attach) echo "$IMG" > "$LUN/file" ;;
  *) echo "usage: nanos-lun.sh {detach|attach}" >&2; exit 1 ;;
esac
