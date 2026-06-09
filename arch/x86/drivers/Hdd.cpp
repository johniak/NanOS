/*
 * Hdd.cpp
 *
 *  Created on: Feb 4, 2014
 *      Author: johniak
 */

#include "Hdd.h"

namespace kernel {


int Hdd::readSectors(int lba, char count, char* buf) {
	return read_ata_pio(lba, count, buf);
}

int Hdd::writeSectors(int lba, char count, char* buf) {
	return write_ata_pio(lba, count, buf);
}

} /* namespace kernel */

