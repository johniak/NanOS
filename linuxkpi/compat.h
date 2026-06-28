/*
 * linuxkpi/compat.h — force-included prelude for every LinuxKPI translation unit (shim
 * sources and, later, the vendored Linux driver/core). Keeps a single consistent base of
 * fixed-width types and the handful of compiler annotations Linux source sprinkles
 * everywhere. Kept deliberately small; subsystem helpers live in the matching <linux/*.h>.
 */
#ifndef _LINUXKPI_COMPAT_H
#define _LINUXKPI_COMPAT_H

#include <linux/types.h>

/* sparse/compiler annotations — no-ops for our toolchain */
#ifndef __user
#define __user
#endif
#ifndef __kernel
#define __kernel
#endif
#ifndef __iomem
#define __iomem
#endif
#ifndef __force
#define __force
#endif
#ifndef __must_check
#define __must_check
#endif
#ifndef __init
#define __init
#endif
#ifndef __exit
#define __exit
#endif
#ifndef __percpu
#define __percpu
#endif

#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#endif /* _LINUXKPI_COMPAT_H */
