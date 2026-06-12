/*
 * Net.h — shared networking primitives: byte-order helpers, wire read/write, and the
 * Internet checksum. All MI, host-testable. The hton/ntoh helpers assume a little-endian
 * host (i686 and the test hosts are LE); for wire I/O prefer the explicit rd/wr big-endian
 * helpers, which are endianness-independent.
 */
#pragma once
#include <stdint.h>

namespace kernel {

inline uint16_t hton16(uint16_t v) { return (uint16_t) ((v >> 8) | (v << 8)); }
inline uint16_t ntoh16(uint16_t v) { return hton16(v); }
inline uint32_t hton32(uint32_t v) {
	return ((v >> 24) & 0xff) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | ((v << 24) & 0xff000000);
}
inline uint32_t ntoh32(uint32_t v) { return hton32(v); }

// Big-endian (network order) access into a byte buffer — correct on any host endianness.
inline uint16_t rd16be(const unsigned char* p) { return (uint16_t) ((p[0] << 8) | p[1]); }
inline uint32_t rd32be(const unsigned char* p) {
	return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}
inline void wr16be(unsigned char* p, uint16_t v) { p[0] = (unsigned char) (v >> 8); p[1] = (unsigned char) v; }
inline void wr32be(unsigned char* p, uint32_t v) {
	p[0] = (unsigned char) (v >> 24); p[1] = (unsigned char) (v >> 16);
	p[2] = (unsigned char) (v >> 8);  p[3] = (unsigned char) v;
}

// Form an IPv4 address (host order) from octets a.b.c.d.
inline uint32_t ipv4(unsigned a, unsigned b, unsigned c, unsigned d) {
	return ((uint32_t) a << 24) | ((uint32_t) b << 16) | ((uint32_t) c << 8) | (uint32_t) d;
}

// The Internet checksum (RFC 1071): one's-complement sum of 16-bit big-endian words over
// `len` bytes, folded and inverted. `init` lets callers chain a pseudo-header. The returned
// value is already in the form to store in the header field (network order, host of the
// 16-bit value). A correct datagram re-checksums to 0.
uint16_t inetChecksum(const void* data, int len, uint32_t init = 0);

// Accumulate into a running one's-complement sum (not yet folded/inverted) — for building a
// checksum across non-contiguous regions (pseudo-header + payload). Fold+invert with
// inetChecksumFinish.
uint32_t inetChecksumAccum(const void* data, int len, uint32_t sum);
uint16_t inetChecksumFinish(uint32_t sum);

// TCP/UDP checksum over the IPv4 pseudo-header {src, dst, 0, proto, segLen} + the segment.
// A correct segment re-checksums to 0.
uint16_t inetPseudoChecksum(uint32_t src, uint32_t dst, uint8_t proto, const void* seg, int segLen);

}  // namespace kernel
