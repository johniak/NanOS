/*
 * Hdd.h
 *
 *  Created on: Feb 4, 2014
 *      Author: johniak
 */

#ifndef HDD_H_
#define HDD_H_

namespace kernel {

// Return 0 on success, non-zero if the drive reported an error (ERR/DF) or DRQ timed out.
extern "C" int read_ata_pio(int lba, char sect_count, char* buf);
extern "C" int write_ata_pio(int lba, char sect_count, char* buf);

class Hdd {
public:
	static int readSectors(int lba, char count, char* buf);    // 0 ok, non-zero I/O error
	static int writeSectors(int lba, char count, char* buf);
};

} /* namespace kernel */

#endif /* HDD_H_ */
