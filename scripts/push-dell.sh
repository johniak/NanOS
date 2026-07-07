#!/usr/bin/env bash
#
# push-dell.sh — push arbitrary host files onto the Kingston stick's ext root partition
# (the same fast debugfs path update-dell.sh uses for kernel.bin + i915.nkext), so the Dell
# image can pick up individual userland programs without a full ~320 MiB reflash.
#
# Usage:  ./scripts/push-dell.sh SRC[:DEST] [SRC[:DEST]...]
#   SRC   host file (e.g. bin/drmtest.nxe)
#   DEST  absolute path on the stick; default /nanos/bin/<basename of SRC>
#   Every pushed file gets mode 0755 (the image-build idiom for .nxe programs).
#
# Examples:
#   ./scripts/push-dell.sh bin/drmtest.nxe                      # -> /nanos/bin/drmtest.nxe
#   ./scripts/push-dell.sh bin/bash.nxe:/nanos/bin/bash.nxe
#
# Env: USB_NAME  media-name match (regex)  (default "Kingston|DataTraveler")
# macOS only (host debugfs from homebrew e2fsprogs); needs sudo for the raw partition.
set -euo pipefail

USB_NAME="${USB_NAME:-Kingston|DataTraveler}"

die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
note() { printf '\033[36m%s\033[0m\n' "$*"; }

[ $# -ge 1 ] || die "usage: $0 SRC[:DEST] [SRC[:DEST]...]"

DBG=""
for c in debugfs /opt/homebrew/opt/e2fsprogs/sbin/debugfs /usr/local/opt/e2fsprogs/sbin/debugfs; do
    command -v "$c" >/dev/null 2>&1 && { DBG="$c"; break; }
done
[ -n "$DBG" ] || die "debugfs not found — 'brew install e2fsprogs'"

# Locate the Kingston stick (external removable disk whose media name matches USB_NAME).
DEV=""
for d in $(diskutil list external physical 2>/dev/null \
           | awk '/^\/dev\/disk[0-9]+ \(external, physical\)/ {print $1}'); do
    name=$(diskutil info "$d" 2>/dev/null | awk -F': *' '/Device \/ Media Name/ {print $2}')
    printf '%s' "$name" | grep -Eiq "$USB_NAME" && { [ -n "$DEV" ] && die "multiple USB matches — unplug extras"; DEV="$d"; NAME="$name"; }
done
[ -n "$DEV" ] || die "no USB disk matching '$USB_NAME' — is the Kingston plugged in?"

PART=$(diskutil list "$DEV" | awk '/Linux Filesystem/ {print $NF}' | tail -1)
[ -n "$PART" ] || die "no Linux Filesystem partition on $DEV"
PDEV="/dev/$PART"

note "stick     : $DEV  ($NAME)"
note "ext part  : $PDEV"

# Resolve SRC[:DEST] pairs up front so a typo dies before any write touches the stick.
SRCS=(); DESTS=()
for spec in "$@"; do
    src="${spec%%:*}"
    dest="${spec#*:}"
    [ "$dest" = "$spec" ] && dest="/nanos/bin/$(basename "$src")"
    [ -f "$src" ] || die "not found: $src"
    case "$dest" in /*) ;; *) die "DEST must be absolute: $dest";; esac
    SRCS+=("$src"); DESTS+=("$dest")
    note "push      : $src  ($(wc -c < "$src" | tr -d ' ') B)  -> $dest"
done

diskutil unmountDisk "$DEV" >/dev/null 2>&1 || true
# Cache sudo up front with a visible prompt (the debugfs calls suppress stderr, which would
# otherwise eat the password prompt and fail silently).
sudo -v || die "sudo required to write the raw partition"

# One debugfs script: rm+write+chmod each file (the exact idiom the image build uses).
CMDS=$(mktemp)
{
    for i in "${!SRCS[@]}"; do
        echo "rm ${DESTS[$i]}"
        echo "write ${SRCS[$i]} ${DESTS[$i]}"
        echo "set_inode_field ${DESTS[$i]} mode 0100755"
    done
} > "$CMDS"
sudo "$DBG" -w -f "$CMDS" "$PDEV" >/dev/null 2>&1 || true
rm -f "$CMDS"

# Verify: on-stick size must match the host artifact (a short write = a truncated binary).
for i in "${!SRCS[@]}"; do
    want=$(wc -c < "${SRCS[$i]}" | tr -d ' ')
    got=$(sudo "$DBG" -R "stat ${DESTS[$i]}" "$PDEV" 2>/dev/null | grep -oE 'Size: [0-9]+' | head -1 | grep -oE '[0-9]+')
    [ "$got" = "$want" ] || die "verify FAILED for ${DESTS[$i]} — on-stick size '$got' != host '$want'"
    note "  ok ${DESTS[$i]}  ($got B)"
done
note "done — eject with: diskutil eject $DEV"
