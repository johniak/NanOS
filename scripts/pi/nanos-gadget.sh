#!/usr/bin/env bash
# nanos-gadget.sh — bring up (or refresh) the NanOS USB mass-storage gadget.
# Idempotent: tears down an existing "nanos" gadget before rebuilding.
set -euo pipefail

IMG="${1:-/home/pi/nanos/image64.img}"
G=/sys/kernel/config/usb_gadget/nanos

[ -f "$IMG" ] || { echo "backing image missing: $IMG" >&2; exit 1; }
modprobe libcomposite

# Idempotent teardown: unbind then remove any existing gadget tree.
if [ -d "$G" ]; then
  echo "" > "$G/UDC" 2>/dev/null || true
  rm -f "$G/configs/c.1/mass_storage.0" 2>/dev/null || true
  rmdir "$G/configs/c.1/strings/0x409" 2>/dev/null || true
  rmdir "$G/configs/c.1" 2>/dev/null || true
  rmdir "$G/functions/mass_storage.0" 2>/dev/null || true
  rmdir "$G/strings/0x409" 2>/dev/null || true
  rmdir "$G" 2>/dev/null || true
fi

mkdir -p "$G"
cd "$G"
echo 0x0525 > idVendor           # NetChip — classic file-backed storage vendor
echo 0xa4a5 > idProduct
echo 0x0100 > bcdDevice
echo 0x0200 > bcdUSB
mkdir -p strings/0x409
echo "NanOS"        > strings/0x409/manufacturer
echo "pendrak boot" > strings/0x409/product
echo "pendrak0001"  > strings/0x409/serialnumber
mkdir -p configs/c.1/strings/0x409
echo "mass_storage" > configs/c.1/strings/0x409/configuration
echo 250 > configs/c.1/MaxPower
mkdir -p functions/mass_storage.0
echo 1     > functions/mass_storage.0/lun.0/removable
echo 0     > functions/mass_storage.0/lun.0/ro
echo 0     > functions/mass_storage.0/lun.0/nofua
echo "$IMG" > functions/mass_storage.0/lun.0/file
ln -sf functions/mass_storage.0 configs/c.1/
ls /sys/class/udc > UDC
echo "gadget up: udc=$(cat UDC) file=$IMG"
