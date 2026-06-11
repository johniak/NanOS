#include "ext/ExtCsum.h"
#include "ext/Crc32c.h"

namespace kernel {

static unsigned rd32(const void* p, unsigned off) {
	const unsigned char* b = (const unsigned char*) p + off;
	return (unsigned) b[0] | ((unsigned) b[1] << 8) | ((unsigned) b[2] << 16) | ((unsigned) b[3] << 24);
}
static unsigned short rd16(const void* p, unsigned off) {
	const unsigned char* b = (const unsigned char*) p + off;
	return (unsigned short) (b[0] | (b[1] << 8));
}

bool extHasMetadataCsum(const void* sb) {
	return (rd32(sb, EXT_SB_FEATURE_ROCOMPAT) & EXT_ROCOMPAT_META_CSUM) != 0;
}

unsigned extCsumSeed(const void* sb) {
	if (rd32(sb, EXT_SB_FEATURE_INCOMPAT) & EXT_INCOMPAT_CSUM_SEED)
		return rd32(sb, EXT_SB_CHECKSUM_SEED);          // stored seed
	return crc32c(0xFFFFFFFFu, (const unsigned char*) sb + EXT_SB_UUID, 16);  // crc32c(~0, uuid)
}

unsigned short extGroupDescCsum(unsigned seed, unsigned groupNo, const void* desc, unsigned descSize) {
	const unsigned char* g = (const unsigned char*) desc;
	unsigned char le_group[4] = {
		(unsigned char) (groupNo), (unsigned char) (groupNo >> 8),
		(unsigned char) (groupNo >> 16), (unsigned char) (groupNo >> 24) };
	unsigned char zero2[2] = { 0, 0 };
	unsigned crc = crc32c(seed, le_group, 4);
	crc = crc32c(crc, g, EXT_GD_CHECKSUM_OFFSET);       // bytes before bg_checksum
	crc = crc32c(crc, zero2, 2);                        // the bg_checksum field as zero
	unsigned off = EXT_GD_CHECKSUM_OFFSET + 2;          // 0x20
	if (off < descSize)
		crc = crc32c(crc, g + off, descSize - off);     // the 64-bit tail (if present)
	return (unsigned short) (crc & 0xFFFF);
}

unsigned extSuperblockCsum(const void* sb) {
	return crc32c(0xFFFFFFFFu, sb, EXT_SB_CHECKSUM);    // everything up to s_checksum
}

unsigned extBitmapCsum(unsigned seed, const void* bitmap, unsigned numBytes) {
	return crc32c(seed, bitmap, numBytes);
}

void extInodeCsum(unsigned seed, unsigned inodeNo, void* inode, unsigned inodeSize) {
	unsigned char* p = (unsigned char*) inode;
	// i_extra_isize (off 0x80, u16) tells how far the large-inode area extends; i_checksum_hi
	// at 0x82 is only present when extra covers it.
	unsigned short extra = inodeSize > 128 ? rd16(p, 0x80) : 0;
	bool hasHi = inodeSize > 128 && extra >= (0x82 - 128 + 2);

	// Save then zero the checksum fields, fold (inode# , i_generation, inode-bytes), restore split.
	unsigned short loSave = rd16(p, 0x7C);
	unsigned short hiSave = hasHi ? rd16(p, 0x82) : 0;
	p[0x7C] = 0; p[0x7D] = 0;
	if (hasHi) { p[0x82] = 0; p[0x83] = 0; }

	unsigned gen = rd32(p, 0x64);                       // i_generation
	unsigned char ino_le[4] = {
		(unsigned char) inodeNo, (unsigned char) (inodeNo >> 8),
		(unsigned char) (inodeNo >> 16), (unsigned char) (inodeNo >> 24) };
	unsigned char gen_le[4] = {
		(unsigned char) gen, (unsigned char) (gen >> 8),
		(unsigned char) (gen >> 16), (unsigned char) (gen >> 24) };
	unsigned crc = crc32c(seed, ino_le, 4);
	crc = crc32c(crc, gen_le, 4);
	crc = crc32c(crc, p, inodeSize);

	(void) loSave; (void) hiSave;
	p[0x7C] = (unsigned char) crc; p[0x7D] = (unsigned char) (crc >> 8);
	if (hasHi) { p[0x82] = (unsigned char) (crc >> 16); p[0x83] = (unsigned char) (crc >> 24); }
}

}  // namespace kernel
