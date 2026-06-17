/*
 * Hdd64.h — x86_64 ATA PIO forwarder (mirror of arch/x86/drivers/Hdd.h).
 * The extern "C" routines live in ATA64.S. Distinct object name (Hdd64.o) so the
 * elf64 object never collides with the i686 Hdd.o in the shared bin/.
 */
#pragma once

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
