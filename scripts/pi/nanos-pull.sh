#!/usr/bin/env bash
# nanos-pull.sh — extract paths from the gadget backing image into the pull staging dir so the
# Mac can scp them out. Read-only mount. Run ONLY while the Dell is OFF: its writes must be
# flushed to the backing file, and a RW view here while the Dell writes would corrupt the fs.
set -euo pipefail

IMG="${IMG:-/home/pi/nanos/image64.img}"
OUT="${OUT:-/home/pi/nanos/pull}"
LUN=/sys/kernel/config/usb_gadget/nanos/functions/mass_storage.0/lun.0
MNT=/mnt/nanos-pull
LOOP=""

note(){ printf '\033[36m%s\033[0m\n' "$*"; }
die(){  printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

cleanup(){
  mountpoint -q "$MNT" && umount "$MNT" || true
  [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null || true
  echo "$IMG" > "$LUN/file" 2>/dev/null || true   # never leave the Dell diskless
}
trap cleanup EXIT

[ "$#" -ge 1 ] || set -- /nanos/logs             # default: pull the logs dir

# detach for a consistent snapshot, loop-mount the ext root READ-ONLY
echo "" > "$LUN/file"
LOOP=$(losetup -f --show -P "$IMG")
udevadm settle 2>/dev/null || sleep 1
ROOT=""
for p in "${LOOP}"p*; do
  [ -b "$p" ] || continue
  case "$(blkid -o value -s TYPE "$p" 2>/dev/null)" in ext2|ext3|ext4) ROOT="$p"; break ;; esac
done
[ -n "$ROOT" ] || die "no ext[2-4] partition in $IMG"
mkdir -p "$MNT"
mount -o ro "$ROOT" "$MNT"

rm -rf "$OUT"; mkdir -p "$OUT"
for path in "$@"; do
  src="$MNT/${path#/}"
  if [ ! -e "$src" ]; then note "  skip $path (not present)"; continue; fi
  dest="$OUT/${path#/}"; mkdir -p "$(dirname "$dest")"
  cp -a "$src" "$dest"
  note "  pulled $path"
done
chown -R pi:pi "$OUT" 2>/dev/null || true
sync
note "pull staged in $OUT"
