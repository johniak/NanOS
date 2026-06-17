#include "doctest.h"
#include "RamBlockDevice.h"
#include <cstring>
#include <cstdint>

using namespace kernel;

TEST_CASE("BlockDevice::readSectors carries a 64-bit lba without truncation") {
	struct Spy : kernel::BlockDevice {
		uint64_t seen = 0;
		int readSectors(uint64_t lba, unsigned, void*) override { seen = lba; return 0; }
		int writeSectors(uint64_t, unsigned, const void*) override { return 0; }
		unsigned sectorSize() override { return 512; }
		const char* name() override { return "spy"; }
	} dev;
	dev.readSectors(0x1234ABCDEull, 1, nullptr);     // lba > 4 G sectors
	CHECK(dev.seen == 0x1234ABCDEull);
}

TEST_CASE("RamBlockDevice reports its geometry and name") {
	char backing[512 * 2] = {0};
	RamBlockDevice dev("ram0", backing, sizeof(backing));
	CHECK(dev.sectorSize() == 512);
	CHECK(strcmp(dev.name(), "ram0") == 0);
}

TEST_CASE("RamBlockDevice reads back the bytes in its buffer") {
	char backing[512 * 4];
	for (int i = 0; i < 512 * 4; i++) backing[i] = (char)(i & 0xFF);
	RamBlockDevice dev("ram0", backing, sizeof(backing));

	char out[512];
	CHECK(dev.readSectors(0, 1, out) == 0);
	CHECK(memcmp(out, backing, 512) == 0);

	CHECK(dev.readSectors(1, 1, out) == 0);
	CHECK(memcmp(out, backing + 512, 512) == 0);

	char out2[1024];
	CHECK(dev.readSectors(1, 2, out2) == 0);     // multi-sector
	CHECK(memcmp(out2, backing + 512, 1024) == 0);
}

TEST_CASE("RamBlockDevice rejects out-of-range access without overrunning") {
	char backing[512 * 2] = {0};
	RamBlockDevice dev("ram0", backing, sizeof(backing));
	char out[512];
	CHECK(dev.readSectors(2, 1, out) < 0);       // only sectors 0,1 exist
	CHECK(dev.readSectors(1, 2, out) < 0);       // would run past the end
}

TEST_CASE("RamBlockDevice writes then reads back, rejecting OOB writes") {
	char backing[512 * 2] = {0};
	RamBlockDevice dev("ram0", backing, sizeof(backing));
	char in[512];
	for (int i = 0; i < 512; i++) in[i] = (char)0xAA;

	CHECK(dev.writeSectors(1, 1, in) == 0);
	char out[512];
	CHECK(dev.readSectors(1, 1, out) == 0);
	CHECK(memcmp(in, out, 512) == 0);

	CHECK(dev.writeSectors(2, 1, in) < 0);       // OOB write rejected
}
