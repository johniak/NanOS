/* linuxkpi/include/linux/const.h — _AC/_AT/UL/ULL constant-annotation macros. */
#ifndef _LINUXKPI_LINUX_CONST_H
#define _LINUXKPI_LINUX_CONST_H
#ifdef __ASSEMBLY__
#define _AC(X,Y) X
#define _AT(T,X) X
#else
#define __AC(X,Y) (X##Y)
#define _AC(X,Y)  __AC(X,Y)
#define _AT(T,X)  ((T)(X))
#endif
#define _UL(x)   (_AC(x, UL))
#define _ULL(x)  (_AC(x, ULL))
#define _BITUL(x)  (_UL(1) << (x))
#define _BITULL(x) (_ULL(1) << (x))
/* True (as an integer constant expression) iff x is a compile-time constant. Same trick as Linux's
 * <linux/const.h>: the type of the ?: differs (void* vs int*) depending on whether x folds to 0. i915
 * gates its overflow-check macros (castable_to_type, __overflows_type) on it. */
#ifndef U64_C
#define U64_C(x)  x ## ULL
#define U32_C(x)  x ## U
#define S64_C(x)  x ## LL
#define S32_C(x)  x
#define ULL(x)    x ## ULL
#define LL(x)     x ## LL
#endif

#ifndef __is_constexpr
#define __is_constexpr(x) \
	(sizeof(int) == sizeof(*(8 ? ((void *)((long)(x) * 0l)) : (int *)8)))
#endif
#endif
