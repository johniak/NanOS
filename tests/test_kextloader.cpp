#include "doctest.h"
#include "NxeLoader.h"
#include <cstring>
#include <cstdint>

// Kernel modules (.nkext) load through the SAME engine as .nxe/.ndl (NxeLoader::loadImage),
// but with two properties the existing nxeloader tests don't exercise: they load at a
// NON-ZERO delta (a kext is placed in a kernel heap buffer, not at its preferred base), and
// their imports are scoped to the "kernel" namespace (resolved against the kernel export
// table). These tests pin that behaviour.

using namespace kernel;

static const unsigned BASE  = 0x10000;
static const unsigned DELTA = 0x5000;          // load offset (kext buffer base - preferred base)
static unsigned A(unsigned off) { return BASE + off; }

// Resolver that ONLY answers for the kernel-scoped symbol, proving the lib namespace is
// delivered correctly: returns an address iff (name, lib) == ("knx_register_irq", "kernel").
static void* kernelResolve(const char* name, const char* lib) {
	if (strcmp(name, "knx_register_irq") == 0 && strcmp(lib, "kernel") == 0)
		return (void*) 0x77770000;
	return 0;
}

// Build a kext-shaped module: one relocation (a self-pointer that must shift by DELTA) and
// one kernel-scoped import (an IAT slot to be patched).
static void buildKext(char* buf, unsigned libOff) {
	memset(buf, 0, 512);
	NxHeader* h = (NxHeader*) buf;
	h->magic = NX_MAGIC;
	h->version = NX_VERSION;
	h->entry = A(0x40);
	h->loadBase = BASE;
	h->imageSize = 512;
	h->bssStart = A(0x140);
	h->bssEnd = A(0x160);                  // 32-byte bss
	h->importTable = A(0x80); h->importCount = 1;
	h->relocTable = A(0xA0);  h->relocCount = 1;
	// reloc: fix up the 8-byte word at 0xB0, which holds an absolute module address
	// (self-pointer); the R_X86_64_64 fixup site is the full 64-bit word.
	*(uint64_t*) (buf + 0xA0) = A(0xB0);   // NxReloc.off
	*(uint64_t*) (buf + 0xB0) = A(0xB0);   // the relocated word (preferred-base value)
	// import: knx_register_irq, slot at 0xC0, source library at libOff (0 = flat).
	NxImport* imp = (NxImport*) (buf + 0x80);
	imp[0].nameOff = A(0xD0);
	imp[0].slotAddr = A(0xC0);
	imp[0].libOff = libOff;
	strcpy(buf + 0xD0, "knx_register_irq");
	strcpy(buf + 0xE8, "kernel");
	buf[0x140] = 0x55; buf[0x15F] = 0x55;  // dirty bss to verify zeroing
}

TEST_CASE("nkext loads at a non-zero delta: relocation shifts module addresses by delta") {
	char buf[512];
	buildKext(buf, A(0xE8));               // import scoped to "kernel"
	nxaddr_t entry = 0;
	REQUIRE(NxeLoader::loadImage(buf, 512, DELTA, kernelResolve, &entry) == 0);
	CHECK(entry == A(0x40) + DELTA);                     // entry relocated
	CHECK(*(uint64_t*) (buf + 0xB0) == A(0xB0) + DELTA); // self-pointer relocated (8-byte)
	CHECK(*(void**) (buf + 0xC0) == (void*) 0x77770000); // kernel import bound
	CHECK(buf[0x140] == 0);                              // bss zeroed
	CHECK(buf[0x15F] == 0);
}

TEST_CASE("nkext import scoped to the wrong/flat namespace does NOT resolve to a kernel symbol") {
	char buf[512];
	buildKext(buf, 0);                     // flat import (lib == ""), not "kernel"
	nxaddr_t entry = 0;
	// The resolver only answers for lib=="kernel", so a flat import is unresolved -> -2.
	CHECK(NxeLoader::loadImage(buf, 512, DELTA, kernelResolve, &entry) == -2);
}
