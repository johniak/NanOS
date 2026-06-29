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

/* Compiler attributes the vendored DRM source uses that our toolchain lacks. */
#ifndef __counted_by
#define __counted_by(member)
#endif
#ifndef __malloc
#define __malloc
#endif
#ifndef __alloc_size
#define __alloc_size(...)
#endif
#ifndef __realloc_size
#define __realloc_size(...)
#endif
#ifndef __assume_aligned
#define __assume_aligned(...)
#endif
#ifndef fallthrough
#define fallthrough __attribute__((__fallthrough__))
#endif
#ifndef __nonstring
#define __nonstring
#endif
#ifndef __cleanup
#define __cleanup(f)
#endif
#define MINORBITS 20
#define MINORMASK ((1U<<20)-1)
#define __deprecated
#define __read_mostly
#define __ro_after_init
#define __initconst
#define oops_in_progress 0
#define might_fault() do{}while(0)
#define BITS_PER_TYPE(t) (sizeof(t)*8)
#define MAX_T(t,a,b) max_t(t,a,b)
#define _THIS_IP_   0UL
#define _RET_IP_    0UL
#define KBUILD_MODNAME "virtio_gpu"

/* preempt/irq context predicates — single-threaded ring-0 bring-up: never in atomic/dbg. */
#define in_atomic()      0
#define in_interrupt()   0
#define in_dbg_master()  0
#define irqs_disabled()  0
#define preempt_disable() do {} while (0)
#define preempt_enable()  do {} while (0)
#define preempt_count()   0

/* rounding helpers beyond linux/kernel.h */
#define DIV_ROUND_CLOSEST(x, d)     (((x) + ((d) / 2)) / (d))
#define DIV_ROUND_CLOSEST_ULL(x, d) DIV_ROUND_CLOSEST((unsigned long long)(x), (d))
#define BUILD_BUG_ON_INVALID(e)     ((void)(sizeof((long)(e))))

/* Kconfig query — canonical kernel IS_ENABLED (CONFIG_* are defined as 1 in autoconf.h). */
#define __ARG_PLACEHOLDER_1 0,
#define __take_second_arg(__ignored, val, ...) val
#define __is_defined(x)        ___is_defined(x)
#define ___is_defined(val)     ____is_defined(__ARG_PLACEHOLDER_##val)
#define ____is_defined(arg1_or_junk) __take_second_arg(arg1_or_junk 1, 0)
#define IS_BUILTIN(option)     __is_defined(option)
#define IS_MODULE(option)      0
#define IS_ENABLED(option)     (IS_BUILTIN(option) || IS_MODULE(option))
#define IS_REACHABLE(option)   IS_BUILTIN(option)
#define O_CLOEXEC 02000000
#define O_RDWR    02
#define O_RDONLY  00

/* printf vector wrapper used by drm_print.h */
#include <stdarg.h>
struct va_format { const char *fmt; va_list *va; };

/* the vendored DRM/virtio source assumes these are always pulled in. */
#include <linux/bug.h>
#include <linux/bitops.h>
#include <linux/list.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/errno.h>
#include <linux/wait.h>
#include <linux/string.h>
#include <linux/uuid.h>
#include <linux/ratelimit.h>
#include <linux/ktime.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/stringify.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/capability.h>
#include <linux/uidgid.h>
#include <linux/set_memory.h>
#include <linux/dma-mapping.h>
#include <linux/fwnode.h>
#include <linux/pagemap.h>
#include <asm/cpufeature.h>
#include <asm/fpu/api.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/jump_label.h>
#include <linux/file.h>

#endif /* _LINUXKPI_COMPAT_H */
