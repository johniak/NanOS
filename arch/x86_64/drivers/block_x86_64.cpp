/*
 * block_x86_64.cpp — x86_64 boot block device factory (<arch/block.h>).
 *
 * The PC boot disk is the primary ATA PIO drive (same as i686). MI Kernel::start
 * calls arch::bootDisk(), registers it with the DeviceManager, and mounts the root
 * filesystem over the portable BlockDevice HAL.
 */
#include <arch/block.h>
#include "AtaBlockDevice64.h"

namespace arch {

kernel::BlockDevice* bootDisk() {
	return new kernel::AtaBlockDevice("hd0");
}

}
