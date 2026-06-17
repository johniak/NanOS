#pragma once
#include "BlockDevice.h"

namespace kernel {

// BlockDevice backed by the ATA PIO driver (Hdd/ATA.S). This is the in-kernel
// disk driver; future NVMe/VirtIO drivers implement the same interface.
class AtaBlockDevice: public BlockDevice {
	const char* devName;
public:
	AtaBlockDevice(const char* name);
	int readSectors(uint64_t lba, unsigned count, void* buf);
	int writeSectors(uint64_t lba, unsigned count, const void* buf);
	unsigned sectorSize();
	const char* name();
};

}
