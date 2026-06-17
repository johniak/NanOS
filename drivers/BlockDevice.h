#pragma once
#include <stdint.h>

namespace kernel {

// Disk-driver HAL. Every block device (ATA, RamDisk, future NVMe/VirtIO)
// implements this interface; filesystems talk only to it, never to a driver.
// Sector-addressed I/O; return 0 on success, <0 on error. `lba` is 64-bit so a
// disk past 2 TiB (>4 G sectors) is addressable without losing the high bits.
class BlockDevice {
public:
	virtual ~BlockDevice() {}
	virtual int readSectors(uint64_t lba, unsigned count, void* buf) = 0;
	virtual int writeSectors(uint64_t lba, unsigned count, const void* buf) = 0;
	virtual unsigned sectorSize() = 0;
	virtual const char* name() = 0;
};

}
