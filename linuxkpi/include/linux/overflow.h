#ifndef _LKPI_OVERFLOW_H
#define _LKPI_OVERFLOW_H
#include <linux/kernel.h>
#define check_add_overflow(a,b,d) __builtin_add_overflow(a,b,d)
#define check_mul_overflow(a,b,d) __builtin_mul_overflow(a,b,d)
#define check_sub_overflow(a,b,d) __builtin_sub_overflow(a,b,d)
static inline __attribute__((unused)) unsigned long array_size(unsigned long a, unsigned long b){ unsigned long r; if(__builtin_mul_overflow(a,b,&r)) return ~0UL; return r; }
static inline __attribute__((unused)) unsigned long array3_size(unsigned long a, unsigned long b, unsigned long c){ return array_size(array_size(a,b),c); }
#define size_mul(a,b) array_size(a,b)
#define size_add(a,b) ((a)+(b))
#endif
