/*
 * linuxkpi/include/linux/hash.h — Fibonacci (golden-ratio) integer hashing, as Linux's <linux/hash.h>.
 * i915 uses hash_32 for its object/handle hash tables. GOLDEN_RATIO_32 may already be defined by
 * <linux/hashtable.h>; guard it.
 */
#ifndef _LKPI_LINUX_HASH_H
#define _LKPI_LINUX_HASH_H

#include <linux/types.h>

#ifndef GOLDEN_RATIO_32
#define GOLDEN_RATIO_32 0x61C88647U
#endif
#ifndef GOLDEN_RATIO_64
#define GOLDEN_RATIO_64 0x61C8864680B583EBull
#endif

static inline u32 hash_32(u32 val, unsigned int bits)          { return (u32)(val * GOLDEN_RATIO_32) >> (32 - bits); }
static inline u32 hash_64(u64 val, unsigned int bits)          { return (u32)((val * GOLDEN_RATIO_64) >> (64 - bits)); }
static inline u32 hash_long(unsigned long val, unsigned int bits) { return hash_64((u64)val, bits); }
static inline u32 hash_ptr(const void *ptr, unsigned int bits) { return hash_long((unsigned long)ptr, bits); }

#endif /* _LKPI_LINUX_HASH_H */
