#include "RamBlockDevice.h"
#include <string.h>

namespace kernel {

static const unsigned SECTOR = 512;

// True if [lba, lba+count) sectors fit within byteSize, overflow-safe.
static bool inRange(unsigned lba, unsigned count, unsigned byteSize) {
	unsigned long long start = (unsigned long long) lba * SECTOR;
	unsigned long long end = start + (unsigned long long) count * SECTOR;
	return end <= (unsigned long long) byteSize;
}

RamBlockDevice::RamBlockDevice(const char* name, void* buffer, unsigned byteSize) {
	this->devName = name;
	this->buffer = (char*) buffer;
	this->byteSize = byteSize;
}

int RamBlockDevice::readSectors(unsigned lba, unsigned count, void* buf) {
	if (!inRange(lba, count, byteSize))
		return -1;
	memcpy(buf, buffer + (unsigned long long) lba * SECTOR, count * SECTOR);
	return 0;
}

int RamBlockDevice::writeSectors(unsigned lba, unsigned count, const void* buf) {
	if (!inRange(lba, count, byteSize))
		return -1;
	memcpy(buffer + (unsigned long long) lba * SECTOR, buf, count * SECTOR);
	return 0;
}

unsigned RamBlockDevice::sectorSize() {
	return SECTOR;
}

const char* RamBlockDevice::name() {
	return devName;
}

}
