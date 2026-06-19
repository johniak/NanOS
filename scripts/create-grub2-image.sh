#!/bin/bash
# Creates the NanOS disk-image skeleton. Runs INSIDE the nanos-build container (Linux);
# no Docker, no mount, no loop device (parted + mtools + mke2fs -E offset + debugfs/limine).
#
# Two layouts, selected by NANOS_BOOT:
#   * NANOS_BOOT=limine  -> hybrid GPT image bootable under BOTH BIOS and UEFI via Limine
#                           (x86_64 live-USB target). bios_boot + ESP/FAT + ext4 root (label NANOS).
#   * (unset, default)   -> legacy GRUB i386-pc MBR image (the frozen i686 path; unchanged).
# The kernel + /nanos tree are populated separately by the Makefile's _image/_image64 (debugfs).

set -e

IMAGE_PATH="${IMAGE_PATH:-disk/image-grub2.img}"

# Skeleton is built once; the kernel + files are (re)written separately each build.
if [ -f "$IMAGE_PATH" ]; then
    echo "Image $IMAGE_PATH already exists. Delete it to recreate."
    exit 0
fi
mkdir -p "$(dirname "$IMAGE_PATH")"

if [ "$NANOS_BOOT" = limine ]; then
    # ---- Hybrid GPT + Limine (BIOS + UEFI) ----
    # Layout (parted aligns to 1 MiB): P1 bios_boot @1MiB(1MiB), P2 ESP/FAT @2MiB(32MiB),
    # P3 ext4 root @34MiB(rest). The P3 byte offset is fixed at 35651584 — the Makefile uses it too.
    ROOT_OFFSET=35651584
    echo "Creating hybrid GPT+Limine image (BIOS+UEFI)..."

    # 320 MiB disk (root ~286 MiB after the 34 MiB boot area).
    dd if=/dev/zero of="$IMAGE_PATH" bs=1M count=320 status=none

    parted -s "$IMAGE_PATH" mklabel gpt
    parted -s "$IMAGE_PATH" mkpart bios_boot 1MiB 2MiB
    parted -s "$IMAGE_PATH" set 1 bios_grub on
    parted -s "$IMAGE_PATH" mkpart ESP fat16 2MiB 34MiB
    parted -s "$IMAGE_PATH" set 2 esp on
    # End the root at 318 MiB (NOT 100%): the last ~2 MiB holds the backup GPT header — letting the
    # ext4 fill to the disk end would overwrite it ("secondary header not valid" on limine install).
    parted -s "$IMAGE_PATH" mkpart NANOS ext4 34MiB 318MiB
    echo "Created GPT partition table (bios_boot + ESP + ext4 root)"

    # ESP (FAT) built in a separate file with mtools, then dd'd into P2 (offset 2 MiB).
    dd if=/dev/zero of=/tmp/esp.img bs=1M count=32 status=none
    mformat -i /tmp/esp.img -v ESP ::
    mmd -i /tmp/esp.img ::/EFI ::/EFI/BOOT
    mcopy -i /tmp/esp.img /usr/local/share/limine/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
    # BIOS stage 2: Limine's bios-install (into the bios_boot partition) still loads limine-bios.sys
    # from a filesystem — it searches the root / boot / limine dirs of a readable partition.
    mcopy -i /tmp/esp.img /usr/local/share/limine/limine-bios.sys ::/limine-bios.sys
    cat > /tmp/limine.conf <<'LCONF'
timeout: 0
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

# ---- Legacy GRUB i386-pc MBR image (frozen i686 path) ----
OFFSET=1048576   # 2048 sectors * 512 bytes = 1MiB
SECTORS=522240   # (256MB - 1MB) / 512  — grown from 32MB to fit large apps (NetSurf ~7MB + res)

echo "Creating GRUB2 HDD image..."

# Embedded GRUB config baked into core.img: hand control to the real grub.cfg
# on the partition. 'normal' gives a full shell with error messages.
EMBED_CFG=$(mktemp)
cat > "$EMBED_CFG" << 'GRUBCFG'
set root=(hd0,msdos1)
set prefix=(hd0,msdos1)/boot/grub
terminal_input console
terminal_output console
normal
GRUBCFG

# 256MB disk image.
dd if=/dev/zero of="$IMAGE_PATH" bs=1M count=256 status=none
echo "Created 256MB disk image"

# MBR partition table, single bootable ext2 partition at 1MiB.
parted -s "$IMAGE_PATH" mklabel msdos
parted -s "$IMAGE_PATH" mkpart primary ext2 1MiB 100%
parted -s "$IMAGE_PATH" set 1 boot on
echo "Created MBR partition table"

# ext2 filesystem at the partition offset (1024-byte blocks).
PART_BLOCKS=$((SECTORS * 512 / 1024))
mke2fs -t ext4 -q -E offset=$OFFSET "$IMAGE_PATH" ${PART_BLOCKS}k
echo "Created ext2 filesystem at offset $OFFSET"

# Populate /boot/grub/grub.cfg via debugfs (no mount needed).
debugfs -w -R "mkdir /boot" "$IMAGE_PATH?offset=$OFFSET"
debugfs -w -R "mkdir /boot/grub" "$IMAGE_PATH?offset=$OFFSET"
debugfs -w -R "write grub.cfg /boot/grub/grub.cfg" "$IMAGE_PATH?offset=$OFFSET"
echo "Populated filesystem via debugfs"

# Install GRUB2: core.img with embedded config + all needed modules.
grub-mkimage -O i386-pc -o /tmp/core.img \
    -c "$EMBED_CFG" \
    -p "(hd0,msdos1)/boot/grub" \
    normal part_msdos ext2 multiboot biosdisk boot configfile

# boot.img -> MBR (first 440 bytes); core.img -> right after the MBR.
dd if=/usr/lib/grub/i386-pc/boot.img of="$IMAGE_PATH" bs=440 count=1 conv=notrunc status=none
dd if=/tmp/core.img of="$IMAGE_PATH" bs=512 seek=1 conv=notrunc status=none
echo "Installed GRUB2 bootloader"

rm -f "$EMBED_CFG" /tmp/core.img

echo ""
echo "Image created: $IMAGE_PATH"
