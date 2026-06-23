#include "acpi_x86_64.h"
#include "Acpi.h"

namespace arch {

// Physical reads go through the huge-page identity map set up by mmuInitKernel (low RAM +
// the BIOS area are identity-mapped). The ACPI tables firmware leaves in low memory are all
// reachable this way, so a direct cast from physical address is valid here.
static bool acpiReadPhys(uint64_t pa, void* dst, uint32_t len) {
	__builtin_memcpy(dst, (const void*) (uintptr_t) pa, len);
	return true;
}

static bool sigRsdp(uint64_t pa) {
	const char* s = "RSD PTR ";
	const char* p = (const char*) (uintptr_t) pa;
	for (int i = 0; i < 8; i++) if (p[i] != s[i]) return false;
	return true;
}

uint64_t acpiFindRsdp() {
	// 1) EBDA: a 16-bit segment value at physical 0x40E; scan its first KiB on 16-byte bounds.
	uint16_t ebdaSeg = *(volatile uint16_t*) (uintptr_t) 0x40E;
	uint64_t ebda = (uint64_t) ebdaSeg << 4;
	if (ebda)
		for (uint64_t a = ebda; a < ebda + 1024; a += 16)
			if (sigRsdp(a)) return a;
	// 2) the BIOS read-only area 0xE0000..0xFFFFF.
	for (uint64_t a = 0xE0000; a < 0x100000; a += 16)
		if (sigRsdp(a)) return a;
	return 0;
}

int acpiEnumCpus(uint8_t* idsOut, int maxOut, uint64_t* lapicAddrOut) {
	uint64_t rsdp = acpiFindRsdp();
	if (!rsdp) return 0;
	kernel::Acpi acpi(acpiReadPhys);
	if (!acpi.parse(rsdp)) return 0;
	int n = acpi.cpuCount();
	if (n > maxOut) n = maxOut;
	for (int i = 0; i < n; i++) idsOut[i] = acpi.lapicId(i);
	if (lapicAddrOut) *lapicAddrOut = acpi.lapicAddr();
	return n;
}

}  // namespace arch
