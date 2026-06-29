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
#define offsetofend(TYPE, MEMBER) (offsetof(TYPE, MEMBER) + sizeof(((TYPE *)0)->MEMBER))
#define sizeof_field(TYPE, MEMBER) (sizeof(((TYPE *)0)->MEMBER))

#define container_of(ptr, type, member) ({                          \
	void *__mptr = (void *)(ptr);                               \
	((type *)(__mptr - offsetof(type, member))); })
#define container_of_const(ptr, type, member) container_of(ptr, type, member)

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define struct_size(p, member, n) (sizeof(*(p)) + (n) * sizeof(*(p)->member))
#define flex_array_size(p, member, n) ((n) * sizeof(*(p)->member))

/* min/max/clamp/swap/abs collide with libstdc++/libc identifiers as function-like macros.
 * On the host doctest path (NANOS_HOST_TEST) they would clobber std::min/std::max/std::swap/
 * std::clamp/abs and break <algorithm>/<atomic>/etc., so suppress them there — the host build
 * never compiles Linux source that needs them (only the C shim primitives, which don't use
 * them). The kext build keeps the kernel-style macros. */
#ifndef NANOS_HOST_TEST
#define min(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#define max(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })
#define clamp(v, lo, hi) max(lo, min(v, hi))
#define swap(a, b) ({ __typeof__(a) __t = (a); (a) = (b); (b) = __t; })
#define abs(x) ({ __typeof__(x) __x = (x); __x < 0 ? -__x : __x; })
#endif
#define min_t(t, a, b) ({ t _a = (t)(a); t _b = (t)(b); _a < _b ? _a : _b; })
#define max_t(t, a, b) ({ t _a = (t)(a); t _b = (t)(b); _a > _b ? _a : _b; })
#define clamp_t(t, v, lo, hi) max_t(t, lo, min_t(t, v, hi))
#define clamp_val(v, lo, hi) clamp_t(__typeof__(v), v, lo, hi)

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
/* INT_MAX/UINT_MAX/SIZE_MAX are glibc <limits.h>/<stdint.h> names. Ours use cast expressions
 * that aren't valid in glibc's `#if INT_MAX == 32767`-style preprocessor checks, so on the host
 * doctest path defer to the real <limits.h>; the freestanding kext build needs ours. */
#ifndef NANOS_HOST_TEST
#define INT_MAX  ((int)(~0U >> 1))
#define UINT_MAX (~0U)
#define SIZE_MAX (~(size_t)0)
#endif

#define upper_32_bits(n) ((u32)(((n) >> 16) >> 16))
#define lower_32_bits(n) ((u32)((n) & 0xffffffff))

#define round_up(x, y)   roundup(x, y)
#define round_down(x, y) rounddown(x, y)

#define do_div(n, base) ({ u32 __rem = (u32)((u64)(n) % (u32)(base)); (n) = (u64)(n) / (u32)(base); __rem; })

static inline u32 reciprocal_scale(u32 val, u32 ep_ro) {
	return (u32)(((u64)val * ep_ro) >> 32);
}

static inline int is_power_of_2(unsigned long n) { return n != 0 && ((n & (n - 1)) == 0); }
static inline unsigned long roundup_pow_of_two(unsigned long n) {
	if (n < 2) return 1;
	return 1UL << (BITS_PER_LONG - __builtin_clzl(n - 1));
}
static inline unsigned long rounddown_pow_of_two(unsigned long n) {
	return 1UL << (BITS_PER_LONG - 1 - __builtin_clzl(n));
}
#define ilog2(n) ((unsigned)(BITS_PER_LONG - 1 - __builtin_clzl((unsigned long)(n))))

#define might_sleep()      do {} while (0)
#define might_sleep_if(c)  do {} while (0)
#define cond_resched()     0
#define cant_sleep()       do {} while (0)

void panic(const char *fmt, ...) __attribute__((noreturn, __format__(__printf__, 1, 2)));

#endif /* _LINUXKPI_LINUX_KERNEL_H */

#ifndef _LKPI_KERNEL_EXTRA
#define _LKPI_KERNEL_EXTRA
#define u64_to_user_ptr(x) ((void *)(unsigned long)(x))
#define typecheck(type,x) 1
static inline const char *str_yes_no(bool v){ return v?"yes":"no"; }
static inline const char *str_on_off(bool v){ return v?"on":"off"; }
static inline const char *str_enabled_disabled(bool v){ return v?"enabled":"disabled"; }
#endif

#ifndef _LKPI_KERNEL_MATH
#define _LKPI_KERNEL_MATH
#define DIV_ROUND_UP_ULL(n,d) DIV_ROUND_UP((unsigned long long)(n),(d))
#define DIV64_U64_ROUND_UP(n,d) DIV_ROUND_UP_ULL(n,d)
#define KHZ2PICOS(a) (1000000000UL/(a))
#endif

#ifndef _LKPI_KERNEL_MIN
#define _LKPI_KERNEL_MIN
#define INT_MIN  (-INT_MAX-1)
#define S32_MIN  (-S32_MAX-1)
#define SHRT_MAX 32767
#define SHRT_MIN (-32768)
#endif
