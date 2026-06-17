#pragma once
#include "BlockDevice.h"

namespace kernel {

// A BlockDevice backed by an in-memory byte buffer. Doubles as the "RamDisk"
// driver and as the host test fixture (load an image into RAM, mount it).
class RamBlockDevice : public BlockDevice {
	const char* devName;
	char* buffer;
	unsigned byteSize;
public:
	RamBlockDevice(const char* name, void* buffer, unsigned byteSize);
	int readSectors(uint64_t lba, unsigned count, void* buf);
	int writeSectors(uint64_t lba, unsigned count, const void* buf);
	unsigned sectorSize();
	const char* name();
};

}
