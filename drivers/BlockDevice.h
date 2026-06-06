#pragma once

namespace kernel {

// Disk-driver HAL. Every block device (ATA, RamDisk, future NVMe/VirtIO)
// implements this interface; filesystems talk only to it, never to a driver.
// Sector-addressed I/O; return 0 on success, <0 on error.
class BlockDevice {
public:
	virtual ~BlockDevice() {}
	virtual int readSectors(unsigned lba, unsigned count, void* buf) = 0;
	virtual int writeSectors(unsigned lba, unsigned count, const void* buf) = 0;
	virtual unsigned sectorSize() = 0;
	virtual const char* name() = 0;
};

}
