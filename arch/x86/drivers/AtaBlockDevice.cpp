#include "AtaBlockDevice.h"
#include "Hdd.h"

namespace kernel {

AtaBlockDevice::AtaBlockDevice(const char* name) {
	this->devName = name;
}

// Issue one ATA command per sector. The PIO routine in ATA.S polls DRQ only
// once per command and then transfers count*256 words; for count > 1 that
// desyncs the drive (the data buffer must be re-serviced per sector), which
// corrupts this and subsequent reads. One sector per command keeps it correct.
// lba is 64-bit at the HAL, but the ATA.S PIO path is LBA28 (Hdd takes an int): the (int)
// cast is a conscious 28-bit truncation — fine for our QEMU disk; LBA48 is future work.
int AtaBlockDevice::readSectors(uint64_t lba, unsigned count, void* buf) {
	char* p = (char*) buf;
	for (unsigned i = 0; i < count; i++)
		if (Hdd::readSectors((int) (lba + i), 1, p + i * 512) != 0)
			return -5;   // -EIO: the drive flagged an error (don't return silent garbage)
	return 0;
}

int AtaBlockDevice::writeSectors(uint64_t lba, unsigned count, const void* buf) {
	char* p = (char*) buf;
	for (unsigned i = 0; i < count; i++)
		if (Hdd::writeSectors((int) (lba + i), 1, p + i * 512) != 0)
			return -5;   // -EIO
	return 0;
}

unsigned AtaBlockDevice::sectorSize() {
	return 512;
}

const char* AtaBlockDevice::name() {
	return devName;
}

}
