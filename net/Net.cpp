#include "Net.h"

namespace kernel {

// Sum 16-bit big-endian words; a trailing odd byte is the high byte of a final word.
uint32_t inetChecksumAccum(const void* data, int len, uint32_t sum) {
	const unsigned char* p = (const unsigned char*) data;
	while (len > 1) {
		sum += rd16be(p);
		p += 2;
		len -= 2;
	}
	if (len > 0)
		sum += (uint32_t) p[0] << 8;   // odd byte = high byte of the last word
	return sum;
}

uint16_t inetChecksumFinish(uint32_t sum) {
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t) ~sum;
}

uint16_t inetChecksum(const void* data, int len, uint32_t init) {
	return inetChecksumFinish(inetChecksumAccum(data, len, init));
}

}  // namespace kernel
