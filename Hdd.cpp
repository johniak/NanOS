/*
 * Hdd.cpp
 *
 *  Created on: Feb 4, 2014
 *      Author: johniak
 */

#include "Hdd.h"

namespace kernel {


void Hdd::readSectors(int lba, char count, char* buf) {
	read_ata_pio(lba,count,buf);
}

void Hdd::writeSectors(int lba, char count, char* buf) {
	write_ata_pio(lba,count,buf);
}

} /* namespace kernel */

