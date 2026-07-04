/*
 * linuxkpi/include/linux/wordpart.h — word-part helpers (6.12 split these out of kernel.h). The
 * shim's kernel.h already defines upper_32_bits/lower_32_bits, so those are #ifndef-guarded; this
 * adds the 16-bit and REPEAT_BYTE helpers drm_fixed.h and i915 need.
 */
#ifndef _LINUXKPI_LINUX_WORDPART_H
#define _LINUXKPI_LINUX_WORDPART_H

#include <linux/types.h>

#ifndef upper_32_bits
#define upper_32_bits(n) ((u32)(((n) >> 16) >> 16))
#endif
#ifndef lower_32_bits
#define lower_32_bits(n) ((u32)((n) & 0xffffffff))
#endif
#ifndef upper_16_bits
#define upper_16_bits(n) ((u16)((n) >> 16))
#endif
#ifndef lower_16_bits
#define lower_16_bits(n) ((u16)((n) & 0xffff))
#endif

#ifndef REPEAT_BYTE
#define REPEAT_BYTE(x)     ((~0ul / 0xff) * (x))
#endif
#ifndef REPEAT_BYTE_U32
#define REPEAT_BYTE_U32(x) lower_32_bits(REPEAT_BYTE(x))
#endif

#ifndef aligned_byte_mask
#define aligned_byte_mask(n) ((1UL << 8 * (n)) - 1)   /* little-endian (x86_64) */
#endif

#endif /* _LINUXKPI_LINUX_WORDPART_H */
