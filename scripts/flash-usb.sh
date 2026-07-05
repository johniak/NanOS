#!/usr/bin/env bash
#
# flash-usb.sh — find the Kingston USB stick and write a NanOS disk image to it.
#
# Locates the removable USB disk whose media name matches $USB_NAME (default
# "Kingston|DataTraveler" — the Dell's boot stick is a Kingston DataTraveler 3.0),
# then dd's $IMAGE onto its RAW device. Refuses to touch internal/fixed disks, refuses
# when zero or more-than-one candidate matches, and requires a typed confirmation
# (unless FORCE=1) because this ERASES the whole stick.
#
# Env:
#   IMAGE     disk image to write        (default disk/image64.img)
#   USB_NAME  media-name match (regex)   (default "Kingston|DataTraveler")
#   FORCE     =1 to skip the y/N prompt  (default unset)
#
# Supports macOS (diskutil) and Linux (lsblk); the user's host is macOS.
set -euo pipefail

IMAGE="${IMAGE:-disk/image64.img}"
USB_NAME="${USB_NAME:-Kingston|DataTraveler}"

die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
note() { printf '\033[36m%s\033[0m\n' "$*"; }

[ -f "$IMAGE" ] || die "image not found: $IMAGE (run 'make image64' first)"
IMG_SIZE=$(wc -c < "$IMAGE" | tr -d ' ')

case "$(uname -s)" in
# ---------------------------------------------------------------- macOS ----
Darwin)
    # Enumerate external physical disks, keep the ones whose media name matches USB_NAME.
    matches=()
    for dev in $(diskutil list external physical 2>/dev/null \
                 | awk '/^\/dev\/disk[0-9]+ \(external, physical\)/ {print $1}'); do
        name=$(diskutil info "$dev" 2>/dev/null | awk -F': *' '/Device \/ Media Name/ {print $2}')
        removable=$(diskutil info "$dev" 2>/dev/null | awk -F': *' '/Removable Media/ {print $2}')
        if printf '%s' "$name" | grep -Eiq "$USB_NAME"; then
            case "$removable" in *[Rr]emovable*|Yes) matches+=("$dev|$name");; esac
        fi
    done
    [ "${#matches[@]}" -eq 0 ] && die "no removable USB disk matching '$USB_NAME' found — is the Kingston plugged in?"
    [ "${#matches[@]}" -gt 1 ] && { printf 'multiple matches:\n'; printf '  %s\n' "${matches[@]}"; die "refusing to guess — unplug the extras"; }

    DEV="${matches[0]%%|*}"
    NAME="${matches[0]#*|}"
    RAW="${DEV/disk/rdisk}"                       # /dev/disk13 -> /dev/rdisk13 (raw = far faster)
    DEV_SIZE=$(diskutil info "$DEV" | awk -F'[()]' '/Disk Size/ {print $2}' | awk '{print $1}')

    note "target : $DEV  ($NAME, ${DEV_SIZE:-?} bytes)"
    note "image  : $IMAGE  (${IMG_SIZE} bytes)"
    [ "$IMG_SIZE" -gt "${DEV_SIZE:-0}" ] 2>/dev/null && die "image larger than the stick — wrong device?"

    if [ "${FORCE:-}" != 1 ]; then
        printf '\033[33mThis ERASES %s (%s). Type "yes" to flash: \033[0m' "$DEV" "$NAME"
        read -r ans; [ "$ans" = yes ] || die "aborted"
    fi

    note "unmounting $DEV ..."
    diskutil unmountDisk "$DEV"
    note "writing (sudo dd -> $RAW, bs=4m; press Ctrl-T for progress) ..."
    sudo dd if="$IMAGE" of="$RAW" bs=4m
    sync
    if [ "${NO_EJECT:-}" = 1 ]; then
        note "leaving $DEV attached (NO_EJECT=1)"
    else
        note "ejecting ..."
        diskutil eject "$DEV" || true
    fi
    ;;

# ---------------------------------------------------------------- Linux ----
Linux)
    matches=()
    while IFS= read -r line; do
        dev=$(printf '%s' "$line" | awk '{print $1}')
        model=$(printf '%s' "$line" | cut -d' ' -f2-)
        printf '%s' "$model" | grep -Eiq "$USB_NAME" && matches+=("/dev/$dev|$model")
    done < <(lsblk -dno NAME,MODEL,TRAN 2>/dev/null | awk '$NF=="usb"')
    [ "${#matches[@]}" -eq 0 ] && die "no USB disk matching '$USB_NAME' found"
    [ "${#matches[@]}" -gt 1 ] && { printf 'multiple matches:\n'; printf '  %s\n' "${matches[@]}"; die "refusing to guess"; }

    DEV="${matches[0]%%|*}"
    NAME="${matches[0]#*|}"
    note "target : $DEV  ($NAME)"
    note "image  : $IMAGE  (${IMG_SIZE} bytes)"
    if [ "${FORCE:-}" != 1 ]; then
        printf '\033[33mThis ERASES %s (%s). Type "yes" to flash: \033[0m' "$DEV" "$NAME"
        read -r ans; [ "$ans" = yes ] || die "aborted"
    fi
    for p in "$DEV"?*; do mountpoint -q "$p" 2>/dev/null && sudo umount "$p" || true; done
    note "writing (sudo dd -> $DEV, bs=4M) ..."
    sudo dd if="$IMAGE" of="$DEV" bs=4M oflag=direct status=progress conv=fsync
    sync
    ;;

*) die "unsupported host OS: $(uname -s)";;
esac

note "done — NanOS flashed to $NAME. Eject-safe; boot the Dell from it."
