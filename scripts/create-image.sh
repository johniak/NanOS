#!/bin/bash
# Creates the NanOS disk-image skeleton. Runs INSIDE the nanos-build container (Linux);
# no Docker, no mount, no loop device (parted + mtools + mke2fs -E offset + debugfs/limine).
#
# One layout: hybrid GPT image bootable under BOTH BIOS and UEFI via Limine (the x86_64
# live-USB target). bios_boot + ESP/FAT + ext4 root (label NANOS). NANOS_BOOT=limine is
# accepted for compatibility; the legacy GRUB i386-pc MBR layout was retired with i686
# (docs/superpowers/plans/2026-07-13-i686-retirement.md).
# The kernel + /nanos tree are populated separately by the Makefile's _image/_image64 (debugfs).

set -e

IMAGE_PATH="${IMAGE_PATH:-disk/image64.img}"

# Skeleton is built once; the kernel + files are (re)written separately each build.
if [ -f "$IMAGE_PATH" ]; then
    echo "Image $IMAGE_PATH already exists. Delete it to recreate."
    exit 0
fi
mkdir -p "$(dirname "$IMAGE_PATH")"

if [ -n "${NANOS_BOOT:-}" ] && [ "$NANOS_BOOT" != limine ]; then
    echo "create-image.sh: unknown NANOS_BOOT='$NANOS_BOOT' (the legacy i686 GRUB layout is retired)" >&2
    exit 1
fi
if true; then
    # ---- Hybrid GPT + Limine (BIOS + UEFI) ----
    # Layout (parted aligns to 1 MiB): P1 bios_boot @1MiB(1MiB), P2 ESP/FAT32 @2MiB(64MiB),
    # P3 ext4 root @66MiB(rest). The P3 byte offset is fixed at 69206016 — the Makefile uses it too.
    # ESP is FAT32 (NOT FAT16): the UEFI spec mandates FAT32 for the ESP, and real firmware (Dell
    # Latitude) silently resets instead of reading a FAT16 ESP — QEMU/OVMF tolerated FAT16, the
    # metal does not. FAT32 needs >=~34 MiB of clusters, hence the 64 MiB ESP.
    ROOT_OFFSET=69206016
    echo "Creating hybrid GPT+Limine image (BIOS+UEFI)..."

    # 320 MiB disk (root ~286 MiB after the 34 MiB boot area).
    dd if=/dev/zero of="$IMAGE_PATH" bs=1M count=320 status=none

    parted -s "$IMAGE_PATH" mklabel gpt
    parted -s "$IMAGE_PATH" mkpart bios_boot 1MiB 2MiB
    parted -s "$IMAGE_PATH" set 1 bios_grub on
    parted -s "$IMAGE_PATH" mkpart ESP fat32 2MiB 66MiB
    parted -s "$IMAGE_PATH" set 2 esp on
    # End the root at 318 MiB (NOT 100%): the last ~2 MiB holds the backup GPT header — letting the
    # ext4 fill to the disk end would overwrite it ("secondary header not valid" on limine install).
    parted -s "$IMAGE_PATH" mkpart NANOS ext4 66MiB 318MiB
    echo "Created GPT partition table (bios_boot + ESP + ext4 root)"

    # ESP (FAT32) built in a separate file with mtools, then dd'd into P2 (offset 2 MiB). -F forces
    # FAT32 (mtools would otherwise pick FAT16 at this size) — required for real UEFI firmware.
    dd if=/dev/zero of=/tmp/esp.img bs=1M count=64 status=none
    mformat -F -i /tmp/esp.img -v ESP ::
    mmd -i /tmp/esp.img ::/EFI ::/EFI/BOOT
    mcopy -i /tmp/esp.img /usr/local/share/limine/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
    # BIOS stage 2: Limine's bios-install (into the bios_boot partition) still loads limine-bios.sys
    # from a filesystem — it searches the root / boot / limine dirs of a readable partition.
    mcopy -i /tmp/esp.img /usr/local/share/limine/limine-bios.sys ::/limine-bios.sys
    cat > /tmp/limine.conf <<'LCONF'
timeout: 5
verbose: yes
serial: yes
/NanOS
    protocol: multiboot1
    path: fslabel(NANOS):/nanos/core/kernel.bin
LCONF
    # Config where BOTH paths look: ESP root (BIOS limine-bios.sys) + next to BOOTX64.EFI (UEFI).
    mcopy -i /tmp/esp.img /tmp/limine.conf ::/limine.conf
    mcopy -i /tmp/esp.img /tmp/limine.conf ::/EFI/BOOT/limine.conf
    dd if=/tmp/esp.img of="$IMAGE_PATH" bs=1M seek=2 conv=notrunc status=none
    echo "Built ESP (BOOTX64.EFI + limine-bios.sys + limine.conf)"

    # ext2 root at P3 (label NANOS so limine.conf's fslabel(NANOS) resolves it). ext2 (block-mapped,
    # NO extents) — Limine's ext reader panics ("block longer than extent") on the extent tree that
    # debugfs lays out for ext4; the kernel auto-detects ext2 and mounts it read+write. Size matches
    # the 34..318 MiB partition exactly so it never runs into the backup GPT at the disk end.
    ROOT_BLOCKS=$(( (318*1024*1024 - ROOT_OFFSET) / 1024 ))
    mke2fs -t ext2 -q -L NANOS -E offset=$ROOT_OFFSET "$IMAGE_PATH" ${ROOT_BLOCKS}k
    echo "Created ext4 root (label NANOS) at offset $ROOT_OFFSET"

    # Limine BIOS stages -> the bios_boot partition (P1).
    limine bios-install "$IMAGE_PATH" 1

    rm -f /tmp/esp.img /tmp/limine.conf
    echo "Hybrid GPT+Limine image created: $IMAGE_PATH (root @ $ROOT_OFFSET)"
    exit 0
fi
