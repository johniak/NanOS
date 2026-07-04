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

static inline int fls(unsigned int x) { return x ? (32 - __builtin_clz(x)) : 0; }
static inline int ffs(int x) { return __builtin_ffs(x); }
static inline unsigned long __ffs(unsigned long x) { return __builtin_ctzl(x); }
static inline unsigned long __fls(unsigned long x) { return x ? (BITS_PER_LONG - 1 - __builtin_clzl(x)) : 0; }
static inline int fls64(u64 x) { return x ? (64 - __builtin_clzll(x)) : 0; }
static inline unsigned int hweight32(u32 w) { return __builtin_popcount(w); }
static inline unsigned int hweight64(u64 w) { return __builtin_popcountll(w); }
static inline unsigned long hweight_long(unsigned long w) { return __builtin_popcountl(w); }

static inline u32 rol32(u32 word, unsigned int shift) { return (word << (shift & 31)) | (word >> ((-shift) & 31)); }
static inline u32 ror32(u32 word, unsigned int shift) { return (word >> (shift & 31)) | (word << ((-shift) & 31)); }

#define order_base_2(n) (fls64((u64)(n) - 1))
static inline int get_count_order(unsigned int count) { return count <= 1 ? 0 : (int)fls(count - 1); }

#endif /* _LINUXKPI_LINUX_BITOPS_H */

#ifndef _LKPI_BITOPS_FOREACH
#define _LKPI_BITOPS_FOREACH
static inline unsigned long find_next_bit(const unsigned long *a, unsigned long sz, unsigned long st){ for(unsigned long i=st;i<sz;i++) if(test_bit(i,a)) return i; return sz; }
static inline unsigned long find_first_bit(const unsigned long *a, unsigned long sz){ return find_next_bit(a,sz,0); }
#define for_each_set_bit(bit,addr,size) for((bit)=find_first_bit((addr),(size)); (bit)<(size); (bit)=find_next_bit((addr),(size),(bit)+1))
#endif

#ifndef _LKPI_BITOPS_UNLOCK
#define _LKPI_BITOPS_UNLOCK
static inline void clear_bit_unlock(long nr, volatile unsigned long *addr){ __atomic_fetch_and(&addr[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_RELEASE); }
static inline void __clear_bit_unlock(long nr, volatile unsigned long *addr){ clear_bit_unlock(nr,addr); }
static inline int test_and_set_bit_lock(long nr, volatile unsigned long *addr){ return test_and_set_bit(nr,addr); }
static inline __s64 sign_extend64(__u64 value, int index){ int shift = 63 - index; return (__s64)(value << shift) >> shift; }
static inline __s32 sign_extend32(__u32 value, int index){ int shift = 31 - index; return (__s32)(value << shift) >> shift; }
#endif
