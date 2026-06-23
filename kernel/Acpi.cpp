#include "Acpi.h"

namespace kernel {

static bool sig4(const unsigned char* h, const char* s) {
	return h[0] == s[0] && h[1] == s[1] && h[2] == s[2] && h[3] == s[3];
}

bool Acpi::checksumOk(uint64_t pa, uint32_t len) {
	uint8_t sum = 0;
	for (uint32_t i = 0; i < len; i++) { uint8_t b; if (!read(pa + i, &b, 1)) return false; sum += b; }
	return sum == 0;
}

// Walk one MADT (APIC table): a 36-byte SDT header + a 4-byte LAPIC address + 4-byte flags,
// then a variable list of [type, length, ...] entries. Collect type-0 (Processor Local APIC)
// entries whose flags bit0 (Enabled) is set; their byte 3 is the APIC id.
bool Acpi::parseMadt(uint64_t pa) {
	unsigned char hdr[44];
	if (!read(pa, hdr, 44)) return false;
	uint32_t len; __builtin_memcpy(&len, hdr + 4, 4);
	if (len < 44) return false;
	if (!checksumOk(pa, len)) return false;
	__builtin_memcpy(&lapicPhys, hdr + 36, 4);          // 32-bit local APIC address field
	uint32_t off = 44;
	while (off + 2 <= len && nCpus < MAX_CPUS) {
		unsigned char e[2]; if (!read(pa + off, e, 2)) return false;
		uint8_t type = e[0], elen = e[1];
		if (elen < 2) break;                            // malformed entry -> stop
		if (type == 0) {                                // Processor Local APIC
			unsigned char le[8]; if (!read(pa + off, le, 8)) return false;
			uint32_t flags; __builtin_memcpy(&flags, le + 4, 4);
			if (flags & 1) ids[nCpus++] = le[3];        // apicId, only if Enabled
		}
		off += elen;
	}
	return nCpus > 0;
}

// Find the MADT by walking the RSDT (rev 0, 32-bit pointers) or XSDT (rev >= 2, 64-bit).
bool Acpi::parse(uint64_t rsdpPhys) {
	unsigned char rsdp[36];                             // extended RSDP is 36 bytes; rev-0 uses first 20
	if (!read(rsdpPhys, rsdp, 36)) return false;
	const char* SIG = "RSD PTR ";
	for (int i = 0; i < 8; i++) if (rsdp[i] != (unsigned char) SIG[i]) return false;
	if (!checksumOk(rsdpPhys, 20)) return false;        // the base RSDP checksum covers 20 bytes
	uint8_t rev = rsdp[15];
	if (rev >= 2) {
		if (!checksumOk(rsdpPhys, 36)) return false;    // the extended RSDP checksum covers 36 bytes
		uint64_t xsdt; __builtin_memcpy(&xsdt, rsdp + 24, 8);   // XSDT address at offset 24
		unsigned char h[36]; if (!read(xsdt, h, 36)) return false;
		uint32_t len; __builtin_memcpy(&len, h + 4, 4);
		if (len < 36 || !checksumOk(xsdt, len)) return false;
		for (uint32_t o = 36; o + 8 <= len; o += 8) {
			uint64_t p; if (!read(xsdt + o, &p, 8)) return false;
			unsigned char th[4]; if (!read(p, th, 4)) return false;
			if (sig4(th, "APIC")) return parseMadt(p);
		}
	} else {
		uint32_t rsdt; __builtin_memcpy(&rsdt, rsdp + 16, 4);   // RSDT address at offset 16
		unsigned char h[36]; if (!read(rsdt, h, 36)) return false;
		uint32_t len; __builtin_memcpy(&len, h + 4, 4);
		if (len < 36 || !checksumOk(rsdt, len)) return false;
		for (uint32_t o = 36; o + 4 <= len; o += 4) {
			uint32_t p; if (!read(rsdt + o, &p, 4)) return false;
			unsigned char th[4]; if (!read(p, th, 4)) return false;
			if (sig4(th, "APIC")) return parseMadt(p);
		}
	}
	return false;
}

}  // namespace kernel
