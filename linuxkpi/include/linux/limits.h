#ifndef _LKPI_LIMITS_H
#define _LKPI_LIMITS_H
#include <linux/kernel.h>
#define USHRT_MAX 0xffffU
#define LONG_MAX  __LONG_MAX__
#define ULONG_MAX (~0UL)
#define LONG_MIN  (-LONG_MAX - 1L)
#define LLONG_MAX  __LONG_LONG_MAX__
#define LLONG_MIN  (-LLONG_MAX - 1LL)
#define ULLONG_MAX (~0ULL)
#define PAGE_SIZE_LIMIT 0
/* signed/unsigned fixed-width limits (Linux <linux/limits.h>). i915 clamps a few values to S16_MAX. */
#ifndef S8_MAX
#define S8_MAX   ((s8)0x7f)
#define S16_MAX  ((s16)0x7fff)
#define S16_MIN  ((s16)(-S16_MAX - 1))
#define S32_MAX  ((s32)0x7fffffff)
#define S32_MIN  ((s32)(-S32_MAX - 1))
#define S64_MAX  ((s64)0x7fffffffffffffffLL)
#define S64_MIN  ((s64)(-S64_MAX - 1))
#define U8_MAX   ((u8)~0U)
#define U16_MAX  ((u16)~0U)
#define U32_MAX  ((u32)~0U)
#define U64_MAX  ((u64)~0ULL)
#endif
#endif
