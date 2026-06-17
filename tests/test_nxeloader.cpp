// Host is arm64/LP64 (no __x86_64__ macro), so force the v4 64-bit format explicitly:
// NX_FORCE64 selects nxaddr_t == uint64_t and NX_VERSION == 4, exercising the x86_64
// loader path (R_X86_64_64 8-byte fixups, 8-byte IAT slots) on the host.
#define NX_FORCE64 1
#include "doctest.h"
#include "NxeLoader.h"
#include <cstring>
#include <cstdint>

using namespace kernel;

static void* fakeResolve(const char* name, const char*) {
	if (strcmp(name, "write") == 0) return (void*) 0x11110000;
	if (strcmp(name, "exit") == 0) return (void*) 0x22220000;
	return 0;   // unknown
}

// Build a valid .nx image in `buf` at the given fake load base. Offsets are fixed and
// non-overlapping within 512 bytes.
static const nxaddr_t BASE = 0x400000;
static nxaddr_t A(nxaddr_t off) { return BASE + off; }

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
	// IAT slots are 8-byte function pointers on x86_64, so space them by 8.
	imp[0].nameOff = A(0xC0); imp[0].slotAddr = A(0x180); imp[0].libOff = 0;   // flat
	imp[1].nameOff = A(0xC8); imp[1].slotAddr = A(0x190); imp[1].libOff = 0;
	buf[0x100] = 0x55; buf[0x13F] = 0x55;   // dirty bss to verify zeroing
}

TEST_CASE("NxeLoader binds imports, zeroes bss, returns entry (v4)") {
	char buf[512];
	buildImage(buf);
	nxaddr_t entry = 0;
	CHECK(NxeLoader::loadImage(buf, 512, 0, fakeResolve, &entry) == 0);
	CHECK(entry == A(0x40));
	CHECK(*(void**) (buf + 0x180) == (void*) 0x11110000);   // write slot (8-byte)
	CHECK(*(void**) (buf + 0x190) == (void*) 0x22220000);   // exit slot (8-byte)
	CHECK(buf[0x100] == 0);                                  // bss zeroed
	CHECK(buf[0x13F] == 0);
}

TEST_CASE("NxeLoader rejects a bad magic") {
	char buf[512];
	buildImage(buf);
	((NxHeader*) buf)->magic = 0xDEAD;
	nxaddr_t entry = 0;
	CHECK(NxeLoader::loadImage(buf, 512, 0, fakeResolve, &entry) < 0);
}

TEST_CASE("NxeLoader rejects a v3 image (clean cut, no back-compat)") {
	char buf[512];
	buildImage(buf);
	((NxHeader*) buf)->version = 3;
	nxaddr_t entry = 0;
	CHECK(NxeLoader::loadImage(buf, 512, 0, fakeResolve, &entry) == -1);
}

TEST_CASE("NxeLoader fails when an import cannot be resolved") {
	char buf[512];
	buildImage(buf);
	strcpy(buf + 0xC8, "nope");   // second import no longer resolvable
	nxaddr_t entry = 0;
	CHECK(NxeLoader::loadImage(buf, 512, 0, fakeResolve, &entry) == -2);
}

TEST_CASE("NxeLoader rejects an out-of-range import table") {
	char buf[512];
	buildImage(buf);
	((NxHeader*) buf)->importTable = BASE + 0x100000;   // way past the image
	nxaddr_t entry = 0;
	CHECK(NxeLoader::loadImage(buf, 512, 0, fakeResolve, &entry) == -3);
}

TEST_CASE("NxeLoader rejects a too-small buffer") {
	char buf[8] = {0};
	nxaddr_t entry = 0;
	CHECK(NxeLoader::loadImage(buf, 8, 0, fakeResolve, &entry) < 0);
}

// Records the (name, lib) pairs the resolver was asked for, and resolves per library.
static char g_lastLib[32];
static void* libResolve(const char* name, const char* lib) {
	strncpy(g_lastLib, lib ? lib : "(null)", 31);
	if (strcmp(lib, "libc.ndl") == 0 && strcmp(name, "printf") == 0) return (void*) 0xAABB0000;
	if (strcmp(lib, "greet.ndl") == 0 && strcmp(name, "hi") == 0)     return (void*) 0xCCDD0000;
	return 0;
}

TEST_CASE("NxeLoader passes each import's library to the resolver (per-DLL namespace)") {
	char buf[512];
	memset(buf, 0, 512);
	NxHeader* h = (NxHeader*) buf;
	h->magic = NX_MAGIC;
	h->version = NX_VERSION;
	h->entry = A(0x40);
	h->loadBase = BASE;
	h->bssStart = A(0x100);
	h->bssEnd = A(0x100);
	strcpy(buf + 0xC0, "printf");
	strcpy(buf + 0xD0, "libc.ndl");
	h->importTable = A(0x80);
	h->importCount = 1;
	NxImport* imp = (NxImport*) (buf + 0x80);
	imp[0].nameOff = A(0xC0); imp[0].slotAddr = A(0x180); imp[0].libOff = A(0xD0);

	nxaddr_t entry = 0;
	CHECK(NxeLoader::loadImage(buf, 512, 0, libResolve, &entry) == 0);
	CHECK(strcmp(g_lastLib, "libc.ndl") == 0);                 // library was passed through
	CHECK(*(void**) (buf + 0x180) == (void*) 0xAABB0000);      // resolved within libc.ndl

	// An import naming a DIFFERENT library for the same symbol must NOT resolve.
	strcpy(buf + 0xD0, "greet.ndl");
	CHECK(NxeLoader::loadImage(buf, 512, 0, libResolve, &entry) == -2);
}

// ---- base relocation -------------------------------------------------------------

// An image with two absolute 8-byte words (holding vaddrs into itself) and a reloc table
// that lists them. Also one export. No imports.
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
	// Two absolute 8-byte address words inside the image (a pointer table at 0x80/0x88 —
	// well past the v4 NxHeader, which is 0x78 bytes once the address fields are 64-bit).
	*(uint64_t*) (buf + 0x80) = A(0x80);   // points at itself
	*(uint64_t*) (buf + 0x88) = A(0x90);   // points elsewhere in the image
	// Reloc table at 0x100, listing the two words above (R_X86_64_64 sites).
	h->relocTable = A(0x100);
	h->relocCount = 2;
	NxReloc* r = (NxReloc*) (buf + 0x100);
	r[0].off = A(0x80);
	r[1].off = A(0x88);
	// One export "go" at A(0x80), name string at 0x120.
	strcpy(buf + 0x120, "go");
	h->exportTable = A(0x110);
	h->exportCount = 1;
	NxExport* e = (NxExport*) (buf + 0x110);
	e[0].nameOff = A(0x120);
	e[0].addr = A(0x80);
}

// Collects exports reported via the onExport callback.
struct ExpCollect {
	char name[8][32];
	nxaddr_t addr[8];
	int n;
};
static void collectExport(void* ctx, const char* name, nxaddr_t addr) {
	ExpCollect* c = (ExpCollect*) ctx;
	strncpy(c->name[c->n], name, 31);
	c->addr[c->n] = addr;
	c->n++;
}

TEST_CASE("NxeLoader applies R_X86_64_64 base relocations with a non-zero delta") {
	char buf[512];
	buildRelocImage(buf);
	const nxaddr_t delta = 0x100000000ULL;   // > 4 GiB: only an 8-byte fixup survives this
	nxaddr_t entry = 0;
	ExpCollect ec = {};
	CHECK(NxeLoader::loadImage(buf, 512, delta, 0, &entry, collectExport, &ec) == 0);
	// Entry and every relocated 8-byte word shift by the full 64-bit delta.
	CHECK(entry == A(0x40) + delta);
	CHECK(*(uint64_t*) (buf + 0x80) == A(0x80) + delta);
	CHECK(*(uint64_t*) (buf + 0x88) == A(0x90) + delta);
	// Export address is relocated; its name was reported during the callback.
	CHECK(ec.n == 1);
	CHECK(ec.addr[0] == A(0x80) + delta);
	CHECK(strcmp(ec.name[0], "go") == 0);
}

TEST_CASE("NxeLoader with delta 0 leaves addresses untouched") {
	char buf[512];
	buildRelocImage(buf);
	nxaddr_t entry = 0;
	CHECK(NxeLoader::loadImage(buf, 512, 0, 0, &entry) == 0);
	CHECK(entry == A(0x40));
	CHECK(*(uint64_t*) (buf + 0x80) == A(0x80));
	CHECK(*(uint64_t*) (buf + 0x88) == A(0x90));
}

// Collects needed-library names reported via forEachNeeded.
struct NeedCollect { char name[8][32]; int n; };
static void collectNeeded(void* ctx, const char* name) {
	NeedCollect* c = (NeedCollect*) ctx;
	strncpy(c->name[c->n], name, 31);
	c->n++;
}

TEST_CASE("NxeLoader forEachNeeded reports needed libraries") {
	char buf[512];
	memset(buf, 0, 512);
	NxHeader* h = (NxHeader*) buf;
	h->magic = NX_MAGIC;
	h->version = NX_VERSION;
	h->loadBase = BASE;
	h->bssStart = A(0x100);
	h->bssEnd = A(0x100);
	strcpy(buf + 0x100, "libc.ndl");
	strcpy(buf + 0x110, "greet.ndl");
	h->neededTable = A(0x120);
	h->neededCount = 2;
	NxNeeded* nd = (NxNeeded*) (buf + 0x120);
	nd[0].nameOff = A(0x100);
	nd[1].nameOff = A(0x110);

	NeedCollect nc = {};
	CHECK(NxeLoader::forEachNeeded(buf, 512, collectNeeded, &nc) == 0);
	CHECK(nc.n == 2);
	CHECK(strcmp(nc.name[0], "libc.ndl") == 0);
	CHECK(strcmp(nc.name[1], "greet.ndl") == 0);
}

TEST_CASE("NxeLoader forEachNeeded is a no-op without a needed table") {
	char buf[512];
	buildImage(buf);   // neededCount == 0
	NeedCollect nc = {};
	CHECK(NxeLoader::forEachNeeded(buf, 512, collectNeeded, &nc) == 0);
	CHECK(nc.n == 0);
}

TEST_CASE("NxeLoader rejects an out-of-range relocation site") {
	char buf[512];
	buildRelocImage(buf);
	NxReloc* r = (NxReloc*) (buf + 0x100);
	r[0].off = BASE + 0x100000;        // far past the image
	nxaddr_t entry = 0;
	CHECK(NxeLoader::loadImage(buf, 512, 0x10000, 0, &entry) == -3);
}

TEST_CASE("NxeLoader rejects an out-of-range export table") {
	char buf[512];
	buildRelocImage(buf);
	((NxHeader*) buf)->exportTable = BASE + 0x100000;   // past the image
	nxaddr_t entry = 0;
	CHECK(NxeLoader::loadImage(buf, 512, 0, 0, &entry) == -3);
}

TEST_CASE("NxeLoader rejects an out-of-range export name") {
	char buf[512];
	buildRelocImage(buf);
	NxExport* e = (NxExport*) (buf + 0x110);
	e[0].nameOff = BASE + 0x100000;                     // name string past the image
	nxaddr_t entry = 0;
	CHECK(NxeLoader::loadImage(buf, 512, 0, 0, &entry) == -3);
}

TEST_CASE("NxeLoader rejects an out-of-range bss range") {
	char buf[512];
	buildImage(buf);
	((NxHeader*) buf)->bssEnd = BASE + 0x100000;        // bss extends past the buffer capacity
	nxaddr_t entry = 0;
	CHECK(NxeLoader::loadImage(buf, 512, 0, fakeResolve, &entry) == -3);
}

TEST_CASE("NxeLoader rejects an import with an out-of-range library name") {
	char buf[512];
	memset(buf, 0, 512);
	NxHeader* h = (NxHeader*) buf;
	h->magic = NX_MAGIC;
	h->version = NX_VERSION;
	h->entry = A(0x40);
	h->loadBase = BASE;
	h->bssStart = A(0x100);
	h->bssEnd = A(0x100);
	strcpy(buf + 0xC0, "printf");
	h->importTable = A(0x80);
	h->importCount = 1;
	NxImport* imp = (NxImport*) (buf + 0x80);
	imp[0].nameOff = A(0xC0); imp[0].slotAddr = A(0x180);
	imp[0].libOff = BASE + 0x100000;                    // library name pointer past the image
	nxaddr_t entry = 0;
	CHECK(NxeLoader::loadImage(buf, 512, 0, libResolve, &entry) == -3);
}

TEST_CASE("NxeLoader forEachNeeded rejects a bad header") {
	char buf[512];
	buildImage(buf);   // valid header, neededCount == 0
	NeedCollect nc = {};
	// Too small.
	CHECK(NxeLoader::forEachNeeded(buf, 8, collectNeeded, &nc) == -1);
	// Bad magic.
	((NxHeader*) buf)->magic = 0xDEAD;
	CHECK(NxeLoader::forEachNeeded(buf, 512, collectNeeded, &nc) == -1);
	// Wrong version (clean cut).
	buildImage(buf);
	((NxHeader*) buf)->version = 3;
	CHECK(NxeLoader::forEachNeeded(buf, 512, collectNeeded, &nc) == -1);
}

TEST_CASE("NxeLoader forEachNeeded rejects an out-of-range needed table / name") {
	char buf[512];
	memset(buf, 0, 512);
	NxHeader* h = (NxHeader*) buf;
	h->magic = NX_MAGIC;
	h->version = NX_VERSION;
	h->loadBase = BASE;
	h->neededTable = BASE + 0x100000;   // table past the image
	h->neededCount = 1;
	NeedCollect nc = {};
	CHECK(NxeLoader::forEachNeeded(buf, 512, collectNeeded, &nc) == -3);

	// Table in range, but the name pointer is out of range.
	strcpy(buf + 0x100, "x");
	h->neededTable = A(0x120);
	NxNeeded* nd = (NxNeeded*) (buf + 0x120);
	nd[0].nameOff = BASE + 0x100000;
	CHECK(NxeLoader::forEachNeeded(buf, 512, collectNeeded, &nc) == -3);
}
