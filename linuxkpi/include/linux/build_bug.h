/*
 * linuxkpi/include/linux/build_bug.h — compile-time assertion macros for the shim.
 */
#ifndef _LINUXKPI_LINUX_BUILD_BUG_H
#define _LINUXKPI_LINUX_BUILD_BUG_H

#define BUILD_BUG_ON_ZERO(e)  (sizeof(struct { int:(-!!(e)); }))
#define BUILD_BUG_ON(cond)    ((void)sizeof(char[1 - 2 * !!(cond)]))
#define BUILD_BUG_ON_MSG(cond, msg) BUILD_BUG_ON(cond)
#define BUILD_BUG_ON_NOT_POWER_OF_2(n) BUILD_BUG_ON((n) == 0 || (((n) & ((n) - 1)) != 0))
#define BUILD_BUG()           BUILD_BUG_ON(1)
#define static_assert(expr, ...) _Static_assert(expr, #expr)

#endif /* _LINUXKPI_LINUX_BUILD_BUG_H */
