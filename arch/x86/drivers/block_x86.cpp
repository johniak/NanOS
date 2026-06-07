/*
 * block_x86.cpp — x86 boot block device factory (<arch/block.h>).
 *
 * The PC boot disk is the primary ATA PIO drive.
 */
#include <arch/block.h>
#include "AtaBlockDevice.h"

namespace arch {

kernel::BlockDevice* bootDisk() {
	return new kernel::AtaBlockDevice("hd0");
}

}
