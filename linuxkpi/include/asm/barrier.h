/*
 * linuxkpi/include/asm/barrier.h — memory barriers for the shim (x86_64).
 */
#ifndef _LINUXKPI_ASM_BARRIER_H
#define _LINUXKPI_ASM_BARRIER_H

#define mb()      __asm__ __volatile__("mfence" ::: "memory")
#define rmb()     __asm__ __volatile__("lfence" ::: "memory")
#define wmb()     __asm__ __volatile__("sfence" ::: "memory")
#define smp_mb()  __asm__ __volatile__("lock; addl $0,-4(%%rsp)" ::: "memory", "cc")
#define smp_rmb() __asm__ __volatile__("" ::: "memory")
#define smp_wmb() __asm__ __volatile__("" ::: "memory")
#define dma_mb()  mb()
#define dma_rmb() __asm__ __volatile__("" ::: "memory")
#define dma_wmb() __asm__ __volatile__("" ::: "memory")
#define __smp_mb()  smp_mb()
#define __smp_rmb() smp_rmb()
#define __smp_wmb() smp_wmb()

#define smp_mb__before_atomic() smp_mb()
#define smp_mb__after_atomic()  smp_mb()

#define smp_store_release(p, v) do { smp_mb(); WRITE_ONCE(*(p), (v)); } while (0)
#define smp_load_acquire(p) ({ __typeof__(*(p)) ___v = READ_ONCE(*(p)); smp_mb(); ___v; })
#define smp_store_mb(var, value) do { WRITE_ONCE(var, value); smp_mb(); } while (0)

static inline void virt_mb(void)  { mb(); }
static inline void virt_rmb(void) { __asm__ __volatile__("" ::: "memory"); }
static inline void virt_wmb(void) { __asm__ __volatile__("" ::: "memory"); }

#endif /* _LINUXKPI_ASM_BARRIER_H */
