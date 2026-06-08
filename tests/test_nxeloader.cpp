#include "doctest.h"
#include "NxeLoader.h"
#include <cstring>

using namespace kernel;

static void* fakeResolve(const char* name) {
	if (strcmp(name, "write") == 0) return (void*) 0x11110000;
	if (strcmp(name, "exit") == 0) return (void*) 0x22220000;
	return 0;   // unknown
}

// Build a valid .nx image in `buf` at the given fake load base. Offsets are fixed and
// non-overlapping within 512 bytes.
static const unsigned BASE = 0x400000;
static unsigned A(unsigned off) { return BASE + off; }

static void buildImage(char* buf) {
	memset(buf, 0, 512);
	NxHeader* h = (NxHeader*) buf;
	h->magic = NX_MAGIC;
	h->version = NX_VERSION;
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

TEST_CASE("NxeLoader binds imports, zeroes bss, returns entry") {
	char buf[512];
	buildImage(buf);
	unsigned entry = 0;
	CHECK(NxeLoader::loadImage(buf, 512, 0, fakeResolve, &entry) == 0);
	CHECK(entry == A(0x40));
	CHECK(*(void**) (buf + 0x180) == (void*) 0x11110000);   // write slot
	CHECK(*(void**) (buf + 0x188) == (void*) 0x22220000);   // exit slot
	CHECK(buf[0x100] == 0);                                  // bss zeroed
	CHECK(buf[0x13F] == 0);
}

TEST_CASE("NxeLoader rejects a bad magic") {
	char buf[512];
	buildImage(buf);
	((NxHeader*) buf)->magic = 0xDEAD;
	unsigned entry = 0;
	CHECK(NxeLoader::loadImage(buf, 512, 0, fakeResolve, &entry) < 0);
}

TEST_CASE("NxeLoader fails when an import cannot be resolved") {
	char buf[512];
	buildImage(buf);
	strcpy(buf + 0xC8, "nope");   // second import no longer resolvable
	unsigned entry = 0;
	CHECK(NxeLoader::loadImage(buf, 512, 0, fakeResolve, &entry) == -2);
}

TEST_CASE("NxeLoader rejects an out-of-range import table") {
	char buf[512];
	buildImage(buf);
	((NxHeader*) buf)->importTable = BASE + 0x100000;   // way past the image
	unsigned entry = 0;
	CHECK(NxeLoader::loadImage(buf, 512, 0, fakeResolve, &entry) == -3);
}

TEST_CASE("NxeLoader rejects a too-small buffer") {
	char buf[8] = {0};
	unsigned entry = 0;
	CHECK(NxeLoader::loadImage(buf, 8, 0, fakeResolve, &entry) < 0);
}

// ---- base relocation -------------------------------------------------------------

// An image with two absolute words (holding vaddrs into itself) and a reloc table that
// lists them. Also one export. No imports.
static void buildRelocImage(char* buf) {
	memset(buf, 0, 512);
	NxHeader* h = (NxHeader*) buf;
	h->magic = NX_MAGIC;
	h->version = NX_VERSION;
	h->entry = A(0x40);
	h->loadBase = BASE;
	h->imageSize = 512;
	h->bssStart = A(0x100);
	h->bssEnd = A(0x100);              // no bss
	// Two absolute address words inside the image (e.g. a pointer table at 0x40/0x44).
	*(unsigned*) (buf + 0x40) = A(0x40);   // points at itself
	*(unsigned*) (buf + 0x44) = A(0x80);   // points elsewhere in the image
	// Reloc table at 0x100, listing the two words above.
	h->relocTable = A(0x100);
	h->relocCount = 2;
	NxReloc* r = (NxReloc*) (buf + 0x100);
	r[0].off = A(0x40);
	r[1].off = A(0x44);
	// One export "go" at A(0x40), name string at 0x120.
	strcpy(buf + 0x120, "go");
	h->exportTable = A(0x110);
	h->exportCount = 1;
	NxExport* e = (NxExport*) (buf + 0x110);
	e[0].nameOff = A(0x120);
	e[0].addr = A(0x40);
}

TEST_CASE("NxeLoader applies base relocations with a non-zero delta") {
	char buf[512];
	buildRelocImage(buf);
	const unsigned delta = 0x10000;
	unsigned entry = 0;
	NxLoaded m;
	CHECK(NxeLoader::loadImage(buf, 512, delta, 0, &entry, &m) == 0);
	// Entry and every relocated word shift by delta.
	CHECK(entry == A(0x40) + delta);
	CHECK(*(unsigned*) (buf + 0x40) == A(0x40) + delta);
	CHECK(*(unsigned*) (buf + 0x44) == A(0x80) + delta);
	// Export address is relocated; its name is reachable via the helper.
	CHECK(m.exportCount == 1);
	CHECK(m.exports[0].addr == A(0x40) + delta);
	CHECK(strcmp(NxeLoader::exportName(m, m.exports[0]), "go") == 0);
}

TEST_CASE("NxeLoader with delta 0 leaves addresses untouched") {
	char buf[512];
	buildRelocImage(buf);
	unsigned entry = 0;
	CHECK(NxeLoader::loadImage(buf, 512, 0, 0, &entry) == 0);
	CHECK(entry == A(0x40));
	CHECK(*(unsigned*) (buf + 0x40) == A(0x40));
	CHECK(*(unsigned*) (buf + 0x44) == A(0x80));
}

TEST_CASE("NxeLoader rejects an out-of-range relocation site") {
	char buf[512];
	buildRelocImage(buf);
	NxReloc* r = (NxReloc*) (buf + 0x100);
	r[0].off = BASE + 0x100000;        // far past the image
	unsigned entry = 0;
	CHECK(NxeLoader::loadImage(buf, 512, 0x10000, 0, &entry) == -3);
}
