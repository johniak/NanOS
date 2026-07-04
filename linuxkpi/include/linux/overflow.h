#ifndef _LKPI_OVERFLOW_H
#define _LKPI_OVERFLOW_H
#include <linux/kernel.h>
#include <linux/const.h>   /* __is_constexpr, for the constant-expression overflow checks below */
#define check_add_overflow(a,b,d) __builtin_add_overflow(a,b,d)
#define check_mul_overflow(a,b,d) __builtin_mul_overflow(a,b,d)
#define check_sub_overflow(a,b,d) __builtin_sub_overflow(a,b,d)
static inline __attribute__((unused)) unsigned long array_size(unsigned long a, unsigned long b){ unsigned long r; if(__builtin_mul_overflow(a,b,&r)) return ~0UL; return r; }
static inline __attribute__((unused)) unsigned long array3_size(unsigned long a, unsigned long b, unsigned long c){ return array_size(array_size(a,b),c); }
#define size_mul(a,b) array_size(a,b)
#define size_add(a,b) ((a)+(b))
/* overflows_type(x, T): true if value x does not fit in type T. Mirrors Linux's __overflows_type
 * (add x to a zero of type T and let the compiler tell us if it overflowed) — exact for both signed
 * and unsigned T, constant or runtime x. i915 gates GEM object sizes and user-extension counts on it. */
#define overflows_type(x, T) ({ typeof(T) __ovft = 0; __builtin_add_overflow((x), __ovft, &__ovft); })

/* Pure integer-constant-expression overflow machinery (mirrors Linux's <linux/overflow.h>). Unlike
 * overflows_type above (a statement-expression, fine for runtime use but not usable in static_assert),
 * these fold to an integer constant expression so they can drive static_assert/BUILD_BUG. */
#ifndef is_signed_type
#define is_signed_type(type)   (((type)(-1)) < (type)1)
#define is_unsigned_type(type) (!is_signed_type(type))
#endif
#ifndef type_max
#define __type_half_max(type) ((type)1 << (8*sizeof(type) - 1 - is_signed_type(type)))
#define type_max(T) ((T)((__type_half_max(T) - 1) + __type_half_max(T)))
#define type_min(T) ((T)((T)-type_max(T)-(T)1))
#endif
#ifndef __overflows_type_constexpr
#define __overflows_type_constexpr(x, T) (		\
	is_unsigned_type(typeof(x)) ?			\
		(x) > type_max(T) :			\
	is_unsigned_type(typeof(T)) ?			\
		(x) < 0 || (x) > type_max(T) :		\
	(x) < type_min(T) || (x) > type_max(T))
#endif
/* castable_to_type(n, T): for a compile-time-constant n, true iff n fits in T; for a runtime n we
 * cannot check at compile time, so it is true (the runtime-safe fallback, as in Linux). i915 uses it
 * in static_assert(castable_to_type(page_index, pgoff_t)). */
#ifndef castable_to_type
#define castable_to_type(n, T) \
	__builtin_choose_expr(__is_constexpr(n), !__overflows_type_constexpr(n, T), 1)
#endif
#endif
