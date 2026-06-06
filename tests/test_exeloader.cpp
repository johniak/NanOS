#include "doctest.h"
#include "ExeLoader.h"
#include <cstring>

using namespace kernel;

static void* fakeResolve(const char* name) {
	if (strcmp(name, "write") == 0) return (void*) 0x11110000;
	if (strcmp(name, "exit") == 0) return (void*) 0x22220000;
	return 0;   // unknown
}

// Build a valid .nx image in `buf` at the given fake load base. Returns nothing;
// offsets are fixed and non-overlapping within 512 bytes.
static const unsigned BASE = 0x400000;
static unsigned A(unsigned off) { return BASE + off; }

static void buildImage(char* buf) {
	memset(buf, 0, 512);
	NxHeader* h = (NxHeader*) buf;
	h->magic = NX_MAGIC;
	h->version = 1;
	h->entry = A(0x40);
	h->loadBase = BASE;
	h->imageSize = 512;
	h->bssStart = A(0x100);
	h->bssEnd = A(0x140);          // 64-byte bss
	h->importTable = A(0x80);
	h->importCount = 2;
	strcpy(buf + 0xC0, "write");
	strcpy(buf + 0xC8, "exit");
	NxImport* imp = (NxImport*) (buf + 0x80);
	imp[0].nameOff = A(0xC0); imp[0].slotAddr = A(0x180);
	imp[1].nameOff = A(0xC8); imp[1].slotAddr = A(0x188);
	buf[0x100] = 0x55; buf[0x13F] = 0x55;   // dirty bss to verify zeroing
}

TEST_CASE("ExeLoader binds imports, zeroes bss, returns entry") {
	char buf[512];
	buildImage(buf);
	unsigned entry = 0;
	CHECK(ExeLoader::loadImage(buf, 512, fakeResolve, &entry) == 0);
	CHECK(entry == A(0x40));
	CHECK(*(void**) (buf + 0x180) == (void*) 0x11110000);   // write slot
	CHECK(*(void**) (buf + 0x188) == (void*) 0x22220000);   // exit slot
	CHECK(buf[0x100] == 0);                                  // bss zeroed
	CHECK(buf[0x13F] == 0);
}

TEST_CASE("ExeLoader rejects a bad magic") {
	char buf[512];
	buildImage(buf);
	((NxHeader*) buf)->magic = 0xDEAD;
	unsigned entry = 0;
	CHECK(ExeLoader::loadImage(buf, 512, fakeResolve, &entry) < 0);
}

TEST_CASE("ExeLoader fails when an import cannot be resolved") {
	char buf[512];
	buildImage(buf);
	strcpy(buf + 0xC8, "nope");   // second import no longer resolvable
	unsigned entry = 0;
	CHECK(ExeLoader::loadImage(buf, 512, fakeResolve, &entry) == -2);
}

TEST_CASE("ExeLoader rejects an out-of-range import table") {
	char buf[512];
	buildImage(buf);
	((NxHeader*) buf)->importTable = BASE + 0x100000;   // way past the image
	unsigned entry = 0;
	CHECK(ExeLoader::loadImage(buf, 512, fakeResolve, &entry) == -3);
}

TEST_CASE("ExeLoader rejects a too-small buffer") {
	char buf[8] = {0};
	unsigned entry = 0;
	CHECK(ExeLoader::loadImage(buf, 8, fakeResolve, &entry) < 0);
}
