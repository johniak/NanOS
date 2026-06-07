/*
 * arch/block.h — MI/MD contract for the boot block device.
 *
 * The platform's boot disk is arch-specific (x86 PC: an ATA PIO drive; another
 * arch: SD/MMC). The arch constructs it; MI registers it with the DeviceManager
 * and mounts the root filesystem over the portable BlockDevice HAL.
 */
#pragma once

namespace kernel { class BlockDevice; }

namespace arch {
kernel::BlockDevice* bootDisk();
}
