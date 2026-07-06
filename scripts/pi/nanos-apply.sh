#!/usr/bin/env bash
# nanos-apply.sh — surgically write staged kernel.bin + i915.nkext into the gadget backing
# image's ext root, re-arm i915, verify, and reattach the LUN. Root-only (loop/mount/configfs).
set -euo pipefail

IMG="${IMG:-/home/pi/nanos/image64.img}"
STAGE="${STAGE:-/home/pi/nanos/staging}"
# arm knob: positional arg wins (survives sudo, which scrubs env), else $ARM, else 1.
ARM="${1:-${ARM:-1}}"
LUN=/sys/kernel/config/usb_gadget/nanos/functions/mass_storage.0/lun.0
MNT=/mnt/nanos-root
LOOP=""

note(){ printf '\033[36m%s\033[0m\n' "$*"; }
die(){  printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

reattach(){
  mountpoint -q "$MNT" && umount "$MNT" || true
  [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null || true
  echo "$IMG" > "$LUN/file" 2>/dev/null || true   # never leave the Dell diskless
}
trap reattach EXIT

[ -f "$STAGE/kernel.bin" ] || die "missing $STAGE/kernel.bin"
[ -f "$STAGE/i915.nkext" ] || die "missing $STAGE/i915.nkext"
[ -f "$IMG" ]              || die "missing image $IMG"

# 1) detach the LUN so the mass-storage function closes its fd
echo "" > "$LUN/file"

# 2) loop-mount the ext root (auto partition nodes; detect the ext2/3/4 family — the NanOS
#    root is created as ext2, so hardcoding ext4 would miss it).
LOOP=$(losetup -f --show -P "$IMG")
udevadm settle 2>/dev/null || sleep 1
ROOTPART=""
for p in "${LOOP}"p*; do
  [ -b "$p" ] || continue
  case "$(blkid -o value -s TYPE "$p" 2>/dev/null)" in
    ext2|ext3|ext4) ROOTPART="$p"; break ;;
  esac
done
[ -n "$ROOTPART" ] || die "no ext[2-4] partition found in $IMG"
mkdir -p "$MNT"
mount "$ROOTPART" "$MNT"

# 3) write the two files + the arm knob
install -D -m0644 "$STAGE/kernel.bin" "$MNT/nanos/core/kernel.bin"
install -D -m0644 "$STAGE/i915.nkext" "$MNT/nanos/kext/i915.nkext"
if [ "$ARM" = 1 ]; then mkdir -p "$MNT/nanos/config"; printf 1 > "$MNT/nanos/config/i915"; fi

# 4) verify sizes (a short write = a truncated kernel that triple-faults the Dell)
check(){ local s="$STAGE/$1" d="$MNT/nanos/$2" want got
  want=$(stat -c%s "$s"); got=$(stat -c%s "$d")
  [ "$want" = "$got" ] || die "size mismatch /nanos/$2: want $want got $got"
  note "  ok /nanos/$2 ($got B)"; }
check kernel.bin core/kernel.bin
check i915.nkext kext/i915.nkext
if [ "$ARM" = 1 ]; then
  [ "$(tr -d '\0' < "$MNT/nanos/config/i915")" = 1 ] || die "arm knob != 1"
  note "  ok /nanos/config/i915 = 1"
fi

sync
note "applied — kernel + i915 written; reattaching LUN. Boot the Dell, then 'make i915-log-pi'."
# umount / losetup -d / reattach happen in the EXIT trap
