/*
 * linuxkpi/compat.h — force-included prelude for every LinuxKPI translation unit (shim
 * sources and, later, the vendored Linux driver/core). Keeps a single consistent base of
 * fixed-width types and the handful of compiler annotations Linux source sprinkles
 * everywhere. Kept deliberately small; subsystem helpers live in the matching <linux/*.h>.
 */
#ifndef _LINUXKPI_COMPAT_H
#define _LINUXKPI_COMPAT_H

#include <linux/types.h>

/* File-scope forward decls so the vendored headers all refer to the SAME struct tag
 * (GCC14 errors if a struct is first introduced inside a prototype's parameter list). */
struct cpumask;
struct device;
struct device_node;
struct fwnode_handle;

/* synchronize_rcu()/rcu markers are used by the vendored virtio_config.h before it would
 * transitively pull RCU; force-include keeps them available everywhere. */
#ifndef NANOS_HOST_TEST
#include <linux/rcupdate.h>
#endif

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

/* Endianness conversions (x86_64 is little-endian: le == identity, be == bswap).
 * Defined here in the force-included prelude so they are available to the vendored
 * byteorder headers without a separate asm/byteorder include. */
static inline u16 __lkpi_swab16(u16 x) { return __builtin_bswap16(x); }
static inline u32 __lkpi_swab32(u32 x) { return __builtin_bswap32(x); }
static inline u64 __lkpi_swab64(u64 x) { return __builtin_bswap64(x); }
#define swab16(x) __lkpi_swab16(x)
#define swab32(x) __lkpi_swab32(x)
#define swab64(x) __lkpi_swab64(x)

#define le16_to_cpu(x) ((u16)(x))
#define le32_to_cpu(x) ((u32)(x))
#define le64_to_cpu(x) ((u64)(x))
#define cpu_to_le16(x) ((__le16)(u16)(x))
#define cpu_to_le32(x) ((__le32)(u32)(x))
#define cpu_to_le64(x) ((__le64)(u64)(x))
#define be16_to_cpu(x) (__builtin_bswap16((u16)(x)))
#define be32_to_cpu(x) (__builtin_bswap32((u32)(x)))
#define be64_to_cpu(x) (__builtin_bswap64((u64)(x)))
#define cpu_to_be16(x) ((__be16)__builtin_bswap16((u16)(x)))
#define cpu_to_be32(x) ((__be32)__builtin_bswap32((u32)(x)))
#define cpu_to_be64(x) ((__be64)__builtin_bswap64((u64)(x)))
#define le16_to_cpus(p) do {} while (0)
#define le32_to_cpus(p) do {} while (0)
#define cpu_to_le16s(p) do {} while (0)
#define cpu_to_le32s(p) do {} while (0)

#endif /* _LINUXKPI_COMPAT_H */
