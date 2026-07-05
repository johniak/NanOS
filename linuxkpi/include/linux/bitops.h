/*
 * linuxkpi/include/linux/bitops.h — bit ops + bitmaps for the shim (atomic via builtins).
 */
#ifndef _LINUXKPI_LINUX_BITOPS_H
#define _LINUXKPI_LINUX_BITOPS_H

#include <linux/types.h>
#include <linux/kernel.h>
#include <asm/barrier.h>

#define BITS_TO_U64(nr)  DIV_ROUND_UP(nr, 64)
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]

static inline void set_bit(long nr, volatile unsigned long *addr) {
	__atomic_fetch_or(&addr[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_SEQ_CST);
}
static inline void clear_bit(long nr, volatile unsigned long *addr) {
	__atomic_fetch_and(&addr[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_SEQ_CST);
}
static inline void change_bit(long nr, volatile unsigned long *addr) {
	__atomic_fetch_xor(&addr[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_SEQ_CST);
}
static inline int test_bit(long nr, const volatile unsigned long *addr) {
	return (addr[BIT_WORD(nr)] >> (nr % BITS_PER_LONG)) & 1UL;
}
static inline int test_and_set_bit(long nr, volatile unsigned long *addr) {
	unsigned long old = __atomic_fetch_or(&addr[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_SEQ_CST);
	return (old & BIT_MASK(nr)) != 0;
}
static inline int test_and_clear_bit(long nr, volatile unsigned long *addr) {
	unsigned long old = __atomic_fetch_and(&addr[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_SEQ_CST);
	return (old & BIT_MASK(nr)) != 0;
}
/* non-atomic variants */
static inline void __set_bit(long nr, volatile unsigned long *addr) { addr[BIT_WORD(nr)] |= BIT_MASK(nr); }
static inline void __clear_bit(long nr, volatile unsigned long *addr) { addr[BIT_WORD(nr)] &= ~BIT_MASK(nr); }
/* non-atomic test-and-set/clear (single-threaded bring-up caller holds the relevant lock) */
static inline int __test_and_set_bit(long nr, volatile unsigned long *addr) { unsigned long m=BIT_MASK(nr); volatile unsigned long *p=&addr[BIT_WORD(nr)]; unsigned long old=*p; *p=old|m; return (old&m)!=0; }
static inline int __test_and_clear_bit(long nr, volatile unsigned long *addr) { unsigned long m=BIT_MASK(nr); volatile unsigned long *p=&addr[BIT_WORD(nr)]; unsigned long old=*p; *p=old&~m; return (old&m)!=0; }

static inline int fls(unsigned int x) { return x ? (32 - __builtin_clz(x)) : 0; }
static inline int ffs(int x) { return __builtin_ffs(x); }
static inline unsigned long __ffs(unsigned long x) { return __builtin_ctzl(x); }
static inline unsigned long __fls(unsigned long x) { return x ? (BITS_PER_LONG - 1 - __builtin_clzl(x)) : 0; }
static inline int fls64(u64 x) { return x ? (64 - __builtin_clzll(x)) : 0; }
static inline unsigned int hweight8(u8 w)   { return __builtin_popcount(w); }
static inline unsigned int hweight16(u16 w)  { return __builtin_popcount(w); }
static inline unsigned int hweight32(u32 w) { return __builtin_popcount(w); }
static inline unsigned int hweight64(u64 w) { return __builtin_popcountll(w); }
static inline unsigned long hweight_long(unsigned long w) { return __builtin_popcountl(w); }

static inline u32 rol32(u32 word, unsigned int shift) { return (word << (shift & 31)) | (word >> ((-shift) & 31)); }
static inline u32 ror32(u32 word, unsigned int shift) { return (word >> (shift & 31)) | (word << ((-shift) & 31)); }

/* Constant-expression form: __builtin_clzll of a constant folds, so this is a valid integer constant
 * expression and can size a bit-field (i915 intel_dp uses link_rate_idx:INTEL_DP_LINK_RATE_IDX_BITS,
 * where the width is order_base_2(...)). Value equals the old fls64(n-1) but with no function call.
 * #ifndef-guarded to agree with the identical constant form in <linux/log2.h>. */
#ifndef order_base_2
#define order_base_2(n) ((n) > 1 ? (int)(64 - __builtin_clzll((unsigned long long)((n) - 1))) : 0)
#endif
static inline int get_count_order(unsigned int count) { return count <= 1 ? 0 : (int)fls(count - 1); }

/* Constant-expression Hamming weight (population count). __builtin_popcountll of a constant folds,
 * so these are valid integer constant expressions (drm_dp_tunnel sizes fields with HWEIGHT64). */
#ifndef HWEIGHT64
#define HWEIGHT8(w)  ((unsigned)__builtin_popcount((unsigned char)(w)))
#define HWEIGHT16(w) ((unsigned)__builtin_popcount((unsigned short)(w)))
#define HWEIGHT32(w) ((unsigned)__builtin_popcount((unsigned)(w)))
#define HWEIGHT64(w) ((unsigned)__builtin_popcountll((unsigned long long)(w)))
#define HWEIGHT(w)   HWEIGHT32(w)
#endif

#endif /* _LINUXKPI_LINUX_BITOPS_H */

#ifndef _LKPI_BITOPS_FOREACH
#define _LKPI_BITOPS_FOREACH
static inline unsigned long find_next_bit(const unsigned long *a, unsigned long sz, unsigned long st){ for(unsigned long i=st;i<sz;i++) if(test_bit(i,a)) return i; return sz; }
static inline unsigned long find_first_bit(const unsigned long *a, unsigned long sz){ return find_next_bit(a,sz,0); }
#define for_each_set_bit(bit,addr,size) for((bit)=find_first_bit((addr),(size)); (bit)<(size); (bit)=find_next_bit((addr),(size),(bit)+1))
static inline unsigned long find_next_zero_bit(const unsigned long *a, unsigned long sz, unsigned long st){ for(unsigned long i=st;i<sz;i++) if(!test_bit(i,a)) return i; return sz; }
static inline unsigned long find_first_zero_bit(const unsigned long *a, unsigned long sz){ return find_next_zero_bit(a,sz,0); }
#define for_each_clear_bit(bit,addr,size) for((bit)=find_first_zero_bit((addr),(size)); (bit)<(size); (bit)=find_next_zero_bit((addr),(size),(bit)+1))
/* conditional bit set/clear: __assign_bit(nr, addr, value). */
static inline void __assign_bit(long nr, volatile unsigned long *addr, int value){ if(value) __set_bit(nr,addr); else __clear_bit(nr,addr); }
static inline void assign_bit(long nr, volatile unsigned long *addr, int value){ if(value) set_bit(nr,addr); else clear_bit(nr,addr); }
#endif

#ifndef _LKPI_BITOPS_UNLOCK
#define _LKPI_BITOPS_UNLOCK
static inline void clear_bit_unlock(long nr, volatile unsigned long *addr){ __atomic_fetch_and(&addr[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_RELEASE); }
static inline void __clear_bit_unlock(long nr, volatile unsigned long *addr){ clear_bit_unlock(nr,addr); }
static inline int test_and_set_bit_lock(long nr, volatile unsigned long *addr){ return test_and_set_bit(nr,addr); }
static inline __s64 sign_extend64(__u64 value, int index){ int shift = 63 - index; return (__s64)(value << shift) >> shift; }
static inline __s32 sign_extend32(__u32 value, int index){ int shift = 31 - index; return (__s32)(value << shift) >> shift; }
#endif

/* The bitmap_* family lives in <linux/bitmap.h>. i915 files reach it transitively through bitops.h
 * (they include <linux/bitops.h>, not bitmap.h), matching Linux's include graph. Pulled AFTER the
 * guard above closes so bitmap.h sees the fully-defined bit ops it builds on. */
#include <linux/bitmap.h>
