/*
 * linuxkpi/include/linux/compiler_types.h — type-level compiler helpers for the shim.
 * In mainline this is pulled by <linux/compiler.h>; here it just augments it.
 */
#ifndef _LINUXKPI_LINUX_COMPILER_TYPES_H
#define _LINUXKPI_LINUX_COMPILER_TYPES_H

#include <linux/compiler.h>
#include <linux/build_bug.h>

#define typeof_member(T, m) __typeof__(((T *)0)->m)
#define __same_type(a, b)   __builtin_types_compatible_p(__typeof__(a), __typeof__(b))
#define __must_be_array(a)  BUILD_BUG_ON_ZERO(__same_type((a), &(a)[0]))

#ifndef __always_inline
#define __always_inline inline __attribute__((__always_inline__))
#endif
#define __no_kasan_or_inline __always_inline
#define __no_sanitize_or_inline __always_inline

#define asm_volatile_goto(x...) asm goto(x)
#define asm_inline asm

#endif /* _LINUXKPI_LINUX_COMPILER_TYPES_H */
