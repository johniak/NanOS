/*
 * linuxkpi/include/linux/kernel.h — the grab-bag of core macros/helpers Linux source
 * expects everywhere: container_of, ARRAY_SIZE, min/max/clamp, ALIGN, DIV_ROUND_UP,
 * BIT/GENMASK, etc. Kept to what the vendored virtio/DRM core actually uses.
 */
#ifndef _LINUXKPI_LINUX_KERNEL_H
#define _LINUXKPI_LINUX_KERNEL_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/build_bug.h>
#include <linux/printk.h>
#include <stdarg.h>

#ifndef offsetof
#define offsetof(TYPE, MEMBER) __builtin_offsetof(TYPE, MEMBER)
#endif

#define container_of(ptr, type, member) ({                          \
	void *__mptr = (void *)(ptr);                               \
	((type *)(__mptr - offsetof(type, member))); })

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define min(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#define max(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })
#define min_t(t, a, b) ({ t _a = (t)(a); t _b = (t)(b); _a < _b ? _a : _b; })
#define max_t(t, a, b) ({ t _a = (t)(a); t _b = (t)(b); _a > _b ? _a : _b; })
#define clamp(v, lo, hi) max(lo, min(v, hi))
#define clamp_t(t, v, lo, hi) max_t(t, lo, min_t(t, v, hi))
#define clamp_val(v, lo, hi) clamp_t(__typeof__(v), v, lo, hi)
#define swap(a, b) ({ __typeof__(a) __t = (a); (a) = (b); (b) = __t; })
#define abs(x) ({ __typeof__(x) __x = (x); __x < 0 ? -__x : __x; })

#define ALIGN_MASK(x, mask) (((x) + (mask)) & ~(mask))
#define ALIGN(x, a)        ALIGN_MASK(x, (__typeof__(x))(a) - 1)
#define ALIGN_DOWN(x, a)   ((x) & ~((__typeof__(x))(a) - 1))
#define IS_ALIGNED(x, a)   (((x) & ((__typeof__(x))(a) - 1)) == 0)
#define PTR_ALIGN(p, a)    ((__typeof__(p))ALIGN((unsigned long)(p), (a)))

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define DIV_ROUND_DOWN_ULL(n, d) ((unsigned long long)(n) / (d))
#define roundup(x, y)      (DIV_ROUND_UP(x, y) * (y))
#define rounddown(x, y)    (((x) / (y)) * (y))

#define BIT(n)             (1UL << (n))
#define BIT_ULL(n)         (1ULL << (n))
#define BIT_MASK(nr)       (1UL << ((nr) % (8 * sizeof(long))))
#define BIT_WORD(nr)       ((nr) / (8 * sizeof(long)))
#define BITS_PER_BYTE      8
#define BITS_PER_LONG      64
#define BITS_PER_LONG_LONG 64
#define BITS_TO_LONGS(nr)  DIV_ROUND_UP(nr, 8 * sizeof(long))
#define GENMASK(h, l)      (((~0UL) << (l)) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#define GENMASK_ULL(h, l)  (((~0ULL) << (l)) & (~0ULL >> (BITS_PER_LONG_LONG - 1 - (h))))

#define U8_MAX   ((u8)~0U)
#define U16_MAX  ((u16)~0U)
#define U32_MAX  ((u32)~0U)
#define U64_MAX  ((u64)~0ULL)
#define S32_MAX  ((s32)(U32_MAX >> 1))
#define INT_MAX  ((int)(~0U >> 1))
#define UINT_MAX (~0U)
#define SIZE_MAX (~(size_t)0)

#define upper_32_bits(n) ((u32)(((n) >> 16) >> 16))
#define lower_32_bits(n) ((u32)((n) & 0xffffffff))

#define round_up(x, y)   roundup(x, y)
#define round_down(x, y) rounddown(x, y)

#define do_div(n, base) ({ u32 __rem = (u32)((u64)(n) % (u32)(base)); (n) = (u64)(n) / (u32)(base); __rem; })

static inline u32 reciprocal_scale(u32 val, u32 ep_ro) {
	return (u32)(((u64)val * ep_ro) >> 32);
}

#define might_sleep()      do {} while (0)
#define might_sleep_if(c)  do {} while (0)
#define cond_resched()     0
#define cant_sleep()       do {} while (0)

void panic(const char *fmt, ...) __attribute__((noreturn, __format__(__printf__, 1, 2)));

#endif /* _LINUXKPI_LINUX_KERNEL_H */
