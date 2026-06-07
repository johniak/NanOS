/*
 * Hdd.h
 *
 *  Created on: Feb 4, 2014
 *      Author: johniak
 */

#ifndef HDD_H_
#define HDD_H_

namespace kernel {

extern "C" void read_ata_pio(int lba, char sect_count, char* buf);
extern "C" void write_ata_pio(int lba, char sect_count, char* buf);

class Hdd {
public:
	static void readSectors(int lba, char count, char* buf);
	static void writeSectors(int lba, char count, char* buf);
};

} /* namespace kernel */

#endif /* HDD_H_ */
