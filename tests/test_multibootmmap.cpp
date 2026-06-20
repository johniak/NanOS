#include "doctest.h"
#include "MultibootInfo.h"
#include "MultibootMmap.h"
#include "FrameAllocator.h"   // FrameAllocator::CAPACITY_BYTES (the top-of-RAM cap)
#include <cstring>
#include <cstdint>

using namespace kernel;

// Collects parseMmapBuffer callbacks for assertions.
struct Collected {
	int count;
	uint64_t base[8];
	uint64_t len[8];
	uint32_t type[8];
};
static void collect(void* ctx, uint64_t base, uint64_t length, uint32_t type) {
	Collected* c = (Collected*) ctx;
	if (c->count < 8) {
		c->base[c->count] = base;
		c->len[c->count] = length;
		c->type[c->count] = type;
	}
	c->count++;
}

// One raw mmap entry, GRUB-style: `size` excludes itself, so stride is size+4.
struct RawEntry { uint32_t size; uint64_t base; uint64_t length; uint32_t type; } __attribute__((packed));

static unsigned buildMmap(char* buf, const RawEntry* entries, int n) {
	unsigned off = 0;
	for (int i = 0; i < n; i++) {
		memcpy(buf + off, &entries[i], sizeof(RawEntry));
		off += entries[i].size + 4;   // stride: size excludes the size field itself
	}
	return off;
}

TEST_CASE("parseMmapBuffer walks every entry with the size+4 stride") {
	char buf[256];
	RawEntry e[3] = {
		{ 20, 0x0,        0x9FC00,    MMAP_TYPE_AVAILABLE },   // low usable
		{ 20, 0x100000,   0x7F00000,  MMAP_TYPE_AVAILABLE },   // ~127MB usable
		{ 20, 0xF0000000, 0x10000000, 2 },                     // reserved (MMIO)
	};
	unsigned len = buildMmap(buf, e, 3);

	Collected c = {};
	parseMmapBuffer(buf, len, &c, collect);
	CHECK(c.count == 3);
	CHECK(c.base[0] == 0x0);
	CHECK(c.len[1] == 0x7F00000);
	CHECK(c.type[1] == MMAP_TYPE_AVAILABLE);
	CHECK(c.type[2] == 2);
}

TEST_CASE("parseMmapBuffer tolerates a variable entry size (size+4 stride)") {
	char buf[256];
	// Mix a 24-byte entry (extra trailing field) with a normal 20-byte one.
	RawEntry e[2] = {
		{ 24, 0x100000, 0x1000, MMAP_TYPE_AVAILABLE },
		{ 20, 0x200000, 0x2000, MMAP_TYPE_AVAILABLE },
	};
	unsigned len = buildMmap(buf, e, 2);

	Collected c = {};
	parseMmapBuffer(buf, len, &c, collect);
	CHECK(c.count == 2);
	CHECK(c.base[0] == 0x100000);
	CHECK(c.base[1] == 0x200000);   // proves the 24-byte stride landed correctly
}

TEST_CASE("parseMmapBuffer with zero length yields no entries") {
	char buf[16] = {0};
	Collected c = {};
	parseMmapBuffer(buf, 0, &c, collect);
	CHECK(c.count == 0);
}

TEST_CASE("highestUsableInBuffer returns the top of the highest usable region") {
	char buf[256];
	RawEntry e[3] = {
		{ 20, 0x0,        0x9FC00,   MMAP_TYPE_AVAILABLE },
		{ 20, 0x100000,   0x7F00000, MMAP_TYPE_AVAILABLE },   // ends at 0x8000000
		{ 20, 0xF0000000, 0x1000,    2 },                      // reserved, ignored
	};
	unsigned len = buildMmap(buf, e, 3);
	CHECK(highestUsableInBuffer(buf, len) == 0x8000000ull);   // 0x100000 + 0x7F00000
}

TEST_CASE("highestUsableInBuffer reports an un-clamped 64-bit top") {
	char buf[64];
	RawEntry e[1] = { { 20, 0xF0000000, 0x40000000, MMAP_TYPE_AVAILABLE } };  // ends at 0x130000000
	unsigned len = buildMmap(buf, e, 1);
	CHECK(highestUsableInBuffer(buf, len) == 0x130000000ull);
}

TEST_CASE("parseMmap no-ops without the mmap flag") {
	MultibootInfo mbi;
	memset(&mbi, 0, sizeof(mbi));
	Collected c = {};
	parseMmap(&mbi, &c, collect);
	CHECK(c.count == 0);
}

TEST_CASE("parseMmap with the flag set walks the referenced buffer (len 0 = none)") {
	MultibootInfo mbi;
	memset(&mbi, 0, sizeof(mbi));
	mbi.flags = MB_FLAG_MMAP;
	mbi.mmap_addr = 0x1000;   // never dereferenced because length is 0
	mbi.mmap_length = 0;
	Collected c = {};
	parseMmap(&mbi, &c, collect);
	CHECK(c.count == 0);
}

TEST_CASE("highestUsableAddr uses the mmap when present (len 0 -> 0)") {
	MultibootInfo mbi;
	memset(&mbi, 0, sizeof(mbi));
	mbi.flags = MB_FLAG_MMAP;
	mbi.mmap_addr = 0x1000;   // never dereferenced because length is 0
	mbi.mmap_length = 0;
	CHECK(highestUsableAddr(&mbi) == 0u);
}

TEST_CASE("highestUsableAddr falls back to mem_upper when no mmap flag") {
	MultibootInfo mbi;
	memset(&mbi, 0, sizeof(mbi));
	mbi.flags = MB_FLAG_MEM;        // mem_* valid, mmap not
	mbi.mem_upper = 0x1FC00;        // 0x1FC00 KB = 0x7F00000, +1MB = 0x8000000
	CHECK(highestUsableAddr(&mbi) == 0x8000000u);
}

TEST_CASE("highestUsableAddr keeps a 64-bit top under the 16 GiB cap (no 4GiB clamp)") {
	MultibootInfo mbi;
	memset(&mbi, 0, sizeof(mbi));
	mbi.flags = MB_FLAG_MEM;
	mbi.mem_upper = 0x500000;       // 0x500000 KB = 0x140000000 (5 GiB) — was wrongly clamped to 4 GiB
	CHECK(highestUsableAddr(&mbi) == 0x100000ull + 0x140000000ull);   // 1 MiB + 5 GiB, un-clamped
}

TEST_CASE("highestUsableAddr caps a huge top at the 16 GiB frame-pool capacity") {
	MultibootInfo mbi;
	memset(&mbi, 0, sizeof(mbi));
	mbi.flags = MB_FLAG_MEM;
	mbi.mem_upper = 0x5000000;      // 0x5000000 KB = 0x1400000000 (80 GiB) > 16 GiB cap
	CHECK(highestUsableAddr(&mbi) == FrameAllocator::CAPACITY_BYTES);
}

TEST_CASE("highestUsableAddr returns 0 when neither mmap nor mem flags are set") {
	MultibootInfo mbi;
	memset(&mbi, 0, sizeof(mbi));
	CHECK(highestUsableAddr(&mbi) == 0u);
}
