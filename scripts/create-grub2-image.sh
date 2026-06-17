#!/bin/bash
# Creates a 256MB HDD image with GRUB2 and an ext2 partition starting at LBA 2048.
# Runs INSIDE the nanos-build container (Linux); no Docker, no mount, no loop device.
# Uses mke2fs -E offset and grub-mkimage + dd so it works without privileges.

set -e

# IMAGE_PATH may be overridden in the environment (the x86_64 staged image build reuses this
# same skeleton at a different path, disk/image64-grub2.img); default = the i686 image.
IMAGE_PATH="${IMAGE_PATH:-disk/image-grub2.img}"
OFFSET=1048576   # 2048 sectors * 512 bytes = 1MiB
SECTORS=522240   # (256MB - 1MB) / 512  — grown from 32MB to fit large apps (NetSurf ~7MB + res)

# Skeleton is built once; the kernel is (re)written separately each build.
if [ -f "$IMAGE_PATH" ]; then
    echo "Image $IMAGE_PATH already exists. Delete it to recreate."
    exit 0
fi

mkdir -p "$(dirname "$IMAGE_PATH")"

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
