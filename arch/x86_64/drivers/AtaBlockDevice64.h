#pragma once
#include <stdint.h>
#include "BlockDevice.h"

namespace kernel {

// BlockDevice backed by the x86_64 ATA PIO driver (Hdd64/ATA64.S). Same MI BlockDevice
// HAL as i686; future NVMe/VirtIO drivers implement the same interface.
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
