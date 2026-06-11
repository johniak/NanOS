#include "Crc32c.h"

namespace kernel {

// Lazily-built 256-entry table for the reflected CRC-32C polynomial 0x82F63B78. Built on first
// use (no global constructors run in the kernel; see CLAUDE.md), guarded by a zero-init flag.
static unsigned g_tbl[256];
static bool g_built = false;

static void buildTable() {
	for (unsigned i = 0; i < 256; i++) {
		unsigned c = i;
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
		g_tbl[i] = c;
	}
	g_built = true;
}

unsigned crc32c(unsigned crc, const void* data, unsigned len) {
	if (!g_built)
		buildTable();
	const unsigned char* p = (const unsigned char*) data;
	for (unsigned i = 0; i < len; i++)
		crc = g_tbl[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
	return crc;
}

}  // namespace kernel
