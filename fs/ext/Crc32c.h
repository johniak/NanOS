/*
 * Crc32c.h — CRC-32C (Castagnoli) checksum, the hash ext4's metadata_csum feature uses for
 * every piece of metadata (superblock, group descriptors, inodes, bitmaps, directory blocks,
 * extent blocks). Pure, no I/O — host-testable.
 *
 * crc32c() is a CONTINUABLE primitive: pass the running value in `crc` to chain regions (ext4
 * seeds with the filesystem UUID, then folds in the object). The init/final-xor conventions
 * are the caller's (the ext4 helpers in Phase 1 seed with the UUID and store the raw running
 * value, matching the Linux ext4_chksum() usage); this file is just the reflected polynomial.
 */
#ifndef EXT_CRC32C_H_
#define EXT_CRC32C_H_

namespace kernel {

// Reflected CRC-32C (poly 0x1EDC6F41 -> reflected 0x82F63B78). Returns the running CRC over
// `data[0..len)` continued from `crc`. For a standalone CRC, seed crc = 0xFFFFFFFF.
unsigned crc32c(unsigned crc, const void* data, unsigned len);

}  // namespace kernel

#endif /* EXT_CRC32C_H_ */
