#pragma once
/*
 * GdtBase.h — pure, host-testable byte-packing for an x86 segment descriptor.
 *
 * An x86 GDT entry is 8 bytes with the base and limit fields scattered across it:
 *   [0,1] limit_lo   [2,3] base_lo   [4] base_mid   [5] access
 *   [6]   flags(hi nibble) | limit_hi(lo nibble)    [7] base_hi
 * These two functions encapsulate just that scatter/gather of the 32-bit base and
 * 20-bit limit (no asm, no GDT-load side effects), so the layout is unit-tested on
 * the host. Gdt::setGate / Gdt::setTlsBase build their real descriptors on top.
 */
namespace kernel_arch {

// Write `base` (32-bit) and `limit` (20-bit) into the raw 8-byte descriptor `d`,
// leaving the access byte (d[5]) and the high flag nibble of d[6] untouched.
inline void gdtPackBase(unsigned char* d, unsigned base, unsigned limit) {
	d[2] = (unsigned char) (base & 0xFF);
	d[3] = (unsigned char) ((base >> 8) & 0xFF);
	d[4] = (unsigned char) ((base >> 16) & 0xFF);
	d[7] = (unsigned char) ((base >> 24) & 0xFF);
	d[0] = (unsigned char) (limit & 0xFF);
	d[1] = (unsigned char) ((limit >> 8) & 0xFF);
	d[6] = (unsigned char) ((d[6] & 0xF0) | ((limit >> 16) & 0x0F));
}

// Reassemble the 32-bit base from a raw 8-byte descriptor `d`.
inline unsigned gdtUnpackBase(const unsigned char* d) {
	return (unsigned) d[2]
	     | ((unsigned) d[3] << 8)
	     | ((unsigned) d[4] << 16)
	     | ((unsigned) d[7] << 24);
}

}
