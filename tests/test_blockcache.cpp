#include "doctest.h"
#include "ext/BlockCache.h"
#include "RamBlockDevice.h"
#include <cstring>

using namespace kernel;

// 64 KiB backing buffer = 64 blocks of 1 KiB. partitionLba 0 (bare image).
static RamBlockDevice* mkdev(char* buf, unsigned n) {
	std::memset(buf, 0, n);
	return new RamBlockDevice("bc", buf, n);
}

TEST_CASE("BlockCache: write-back stages in cache, flush persists to the device") {
	char back[64 * 1024];
	RamBlockDevice* dev = mkdev(back, sizeof back);
	BlockCache bc(dev, 0, 1024);

	unsigned char blk[1024];
	std::memset(blk, 0xAB, sizeof blk);
	bc.write(5, blk);

	// Not yet on the device (write-back): the backing bytes for block 5 are still zero.
	CHECK(back[5 * 1024] == 0);
	bc.flush();
	CHECK((unsigned char) back[5 * 1024] == 0xAB);
	CHECK((unsigned char) back[5 * 1024 + 1023] == 0xAB);

	// Read back through a fresh cache reads the persisted bytes.
	BlockCache bc2(dev, 0, 1024);
	unsigned char out[1024];
	bc2.read(5, out);
	CHECK(out[0] == 0xAB);
	delete dev;
}

TEST_CASE("BlockCache: read sees a prior cached write without a device round-trip") {
	char back[64 * 1024];
	RamBlockDevice* dev = mkdev(back, sizeof back);
	BlockCache bc(dev, 0, 1024);

	unsigned char blk[1024];
	std::memset(blk, 0x5A, sizeof blk);
	bc.write(7, blk);
	unsigned char out[1024];
	bc.read(7, out);                 // hits the dirty cache slot
	CHECK(out[0] == 0x5A);
	CHECK(back[7 * 1024] == 0);       // still not flushed
	delete dev;
}

TEST_CASE("BlockCache: writePartial does read-modify-write of a sub-range") {
	char back[64 * 1024];
	RamBlockDevice* dev = mkdev(back, sizeof back);
	BlockCache bc(dev, 0, 1024);

	unsigned char four[4] = { 1, 2, 3, 4 };
	bc.writePartial(3, 100, four, 4);
	bc.flush();
	CHECK((unsigned char) back[3 * 1024 + 100] == 1);
	CHECK((unsigned char) back[3 * 1024 + 103] == 4);
	CHECK((unsigned char) back[3 * 1024 + 99] == 0);    // bytes outside the range untouched
	CHECK((unsigned char) back[3 * 1024 + 104] == 0);
	delete dev;
}

TEST_CASE("BlockCache: eviction flushes a dirty victim (more blocks than slots)") {
	char back[256 * 1024];
	RamBlockDevice* dev = mkdev(back, sizeof back);
	BlockCache bc(dev, 0, 1024);     // 32 slots

	// Dirty 40 distinct blocks (> 32 slots) so the LRU ones get evicted; each eviction must
	// write the dirty victim to the device. After touching all, flush the rest.
	for (unsigned b = 0; b < 40; b++) {
		unsigned char blk[1024];
		std::memset(blk, (int) (b + 1), sizeof blk);
		bc.write(b, blk);
	}
	bc.flush();
	for (unsigned b = 0; b < 40; b++)
		CHECK((unsigned char) back[b * 1024] == (unsigned char) (b + 1));
	delete dev;
}

TEST_CASE("BlockCache: partitionLba offsets every access") {
	char back[64 * 1024];
	RamBlockDevice* dev = mkdev(back, sizeof back);
	BlockCache bc(dev, 8, 1024);     // fs block 0 == device LBA 8 (8*512 = 4096 bytes in)

	unsigned char blk[1024];
	std::memset(blk, 0x77, sizeof blk);
	bc.write(0, blk);
	bc.flush();
	CHECK(back[0] == 0);                         // before the partition: untouched
	CHECK((unsigned char) back[8 * 512] == 0x77); // fs block 0 lands at LBA 8
	delete dev;
}
