/*
 * ExtCsum.h — ext4 metadata_csum helpers built on crc32c. ext4 protects each metadata object
 * with a CRC-32C; getting the seed, field offsets and zeroed-checksum windows byte-exact is the
 * only way e2fsck/Linux will accept what we write. These functions mirror Linux's ext4_chksum
 * usage exactly and are verified against a real metadata_csum image in the tests.
 *
 * The seed: with the metadata_csum_seed incompat feature the filesystem stores it in the
 * superblock (s_checksum_seed); otherwise it is crc32c(~0, s_uuid, 16). extCsumSeed() picks the
 * right one from the superblock bytes.
 */
#ifndef EXT_EXTCSUM_H_
#define EXT_EXTCSUM_H_

namespace kernel {

// Superblock field offsets we read here (Linux ext4 layout).
enum {
	EXT_SB_FEATURE_INCOMPAT = 0x60,   // metadata_csum_seed = bit 0x2000
	EXT_SB_FEATURE_ROCOMPAT = 0x64,   // metadata_csum      = bit 0x400
	EXT_SB_UUID             = 0x68,   // 16 bytes
	EXT_SB_CHECKSUM_SEED    = 0x270,  // u32, valid iff metadata_csum_seed
	EXT_SB_CHECKSUM         = 0x3FC,  // u32, the superblock's own crc32c
	EXT_INCOMPAT_CSUM_SEED  = 0x2000,
	EXT_ROCOMPAT_META_CSUM  = 0x400,
	EXT_GD_CHECKSUM_OFFSET  = 0x1E    // bg_checksum within a group descriptor
};

bool     extHasMetadataCsum(const void* sb);                 // ro_compat metadata_csum set?
unsigned extCsumSeed(const void* sb);                        // per metadata_csum_seed vs UUID

// crc32c-low(16) of a group descriptor, per ext4_group_desc_csum (bg_checksum zeroed in the
// window). `desc` points at the on-disk descriptor bytes; `descSize` is s_desc_size (32 or 64).
unsigned short extGroupDescCsum(unsigned seed, unsigned groupNo, const void* desc, unsigned descSize);

// The superblock's own checksum = crc32c(~0, sb, 0x3FC) (NOT the UUID seed).
unsigned extSuperblockCsum(const void* sb);

// Bitmap checksum (block or inode bitmap): crc32c(seed, bitmap, blockSize). Returns the full
// 32-bit value; callers store the low 16 in bg_*_bitmap_csum_lo and (if desc_size>=64) the
// high 16 in *_csum_hi.
unsigned extBitmapCsum(unsigned seed, const void* bitmap, unsigned blockSize);

// Inode checksum: compute and WRITE i_checksum_lo (off 0x7C) and, if the inode is large enough
// (i_extra_isize covers it), i_checksum_hi (off 0x82). `inode` points at the full on-disk inode
// bytes (inodeSize). Mirrors ext4_inode_csum: seed folded with the inode number + i_generation,
// then the inode with both checksum fields zeroed.
void extInodeCsum(unsigned seed, unsigned inodeNo, void* inode, unsigned inodeSize);

}  // namespace kernel

#endif /* EXT_EXTCSUM_H_ */
