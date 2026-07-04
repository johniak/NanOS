/* linuxkpi/include/linux/unaligned.h — unaligned LE/BE access (VBT parsing). x86_64 permits
 * unaligned loads; these use byte assembly to stay endian-explicit. */
#ifndef _LINUXKPI_LINUX_UNALIGNED_H
#define _LINUXKPI_LINUX_UNALIGNED_H
#include <linux/types.h>
static inline u16 get_unaligned_le16(const void *p){ const u8 *b=p; return b[0] | (b[1]<<8); }
static inline u32 get_unaligned_le32(const void *p){ const u8 *b=p; return b[0] | (b[1]<<8) | (b[2]<<16) | ((u32)b[3]<<24); }
static inline u64 get_unaligned_le64(const void *p){ return (u64)get_unaligned_le32(p) | ((u64)get_unaligned_le32((const u8*)p+4)<<32); }
static inline u16 get_unaligned_be16(const void *p){ const u8 *b=p; return (b[0]<<8) | b[1]; }
static inline u32 get_unaligned_be32(const void *p){ const u8 *b=p; return ((u32)b[0]<<24) | (b[1]<<16) | (b[2]<<8) | b[3]; }
static inline void put_unaligned_le16(u16 v, void *p){ u8 *b=p; b[0]=v; b[1]=v>>8; }
static inline void put_unaligned_le32(u32 v, void *p){ u8 *b=p; b[0]=v; b[1]=v>>8; b[2]=v>>16; b[3]=v>>24; }
#endif
