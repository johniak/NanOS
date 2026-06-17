#include "Hdd64.h"

namespace kernel {

int Hdd::readSectors(int lba, char count, char* buf) {
	return read_ata_pio(lba, count, buf);
}

int Hdd::writeSectors(int lba, char count, char* buf) {
	return write_ata_pio(lba, count, buf);
}

} /* namespace kernel */
