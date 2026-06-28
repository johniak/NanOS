/*
 * linuxkpi/include/linux/compiler.h — compiler glue for the LinuxKPI shim.
 * Provides READ_ONCE/WRITE_ONCE, barrier(), and the annotation macros Linux source uses.
 */
#ifndef _LINUXKPI_LINUX_COMPILER_H
#define _LINUXKPI_LINUX_COMPILER_H

#ifndef __ASSEMBLY__

#define barrier()        __asm__ __volatile__("" ::: "memory")

#define READ_ONCE(x)     (*(const volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof__(x) *)&(x) = (v))

#define __READ_ONCE(x)   READ_ONCE(x)
#define __WRITE_ONCE(x, v) WRITE_ONCE(x, v)

#define ACCESS_ONCE(x)   READ_ONCE(x)

#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#define unlikely_notrace(x) unlikely(x)
#define likely_notrace(x)   likely(x)

#ifndef __force
#define __force
#endif
#ifndef __iomem
#define __iomem
#endif
#ifndef __user
#define __user
#endif
#ifndef __kernel
#define __kernel
#endif
#ifndef __percpu
#define __percpu
#endif
#ifndef __rcu
#define __rcu
#endif
#ifndef __bitwise
#define __bitwise
#endif
#ifndef __must_check
#define __must_check
#endif

#define __must_hold(x)
#define __acquires(x)
#define __releases(x)
#define __acquire(x)   (void)0
#define __release(x)   (void)0
#define __cond_lock(x, c) (c)

#define noinline       __attribute__((__noinline__))
#define __always_inline inline __attribute__((__always_inline__))
#define notrace        __attribute__((__no_instrument_function__))
#define __maybe_unused __attribute__((__unused__))
#define __always_unused __attribute__((__unused__))
#define fallthrough    __attribute__((__fallthrough__))
#define __printf(a, b) __attribute__((__format__(__printf__, a, b)))

#define __aligned(x)   __attribute__((__aligned__(x)))
#define __packed       __attribute__((__packed__))
#define ____cacheline_aligned __attribute__((__aligned__(64)))
#define ____cacheline_aligned_in_smp ____cacheline_aligned

#define __section(s)   __attribute__((__section__(s)))
#define __used         __attribute__((__used__))

#define OPTIMIZER_HIDE_VAR(var) __asm__ __volatile__("" : "+r" (var))

#define data_race(expr) ({ __auto_type __v = ({ expr; }); __v; })

static inline void __chk_user_ptr(const volatile void *p) { (void)p; }

#endif /* __ASSEMBLY__ */
#endif /* _LINUXKPI_LINUX_COMPILER_H */
