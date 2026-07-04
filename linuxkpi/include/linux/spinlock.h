/*
 * linuxkpi/include/linux/spinlock.h — spinlocks for the shim.
 *
 * The lock itself is a UP-cooperative NO-OP (see __lk_acquire below). What makes _irqsave real:
 * on the target it saves RFLAGS and CLIs, so a hard-IRQ handler (the MSI wired in Task 2) cannot
 * preempt a critical section on the same CPU — the textbook UP IRQ-vs-thread exclusion. The plain
 * lock stays a no-op so the cooperative vq pump can re-enter the driver under a "held" lock without
 * self-deadlock. (This is single-core-correct: the virtio-gpu/DRM kext only ever runs on -smp 1;
 * true cross-CPU exclusion for real-HW SMP is a Phase-B concern, flagged in the i915 plan.)
 */
#ifndef _LINUXKPI_LINUX_SPINLOCK_H
#define _LINUXKPI_LINUX_SPINLOCK_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <asm/barrier.h>

typedef struct raw_spinlock { volatile int lock; } raw_spinlock_t;
typedef struct { raw_spinlock_t rlock; } spinlock_t;

#define __RAW_SPIN_LOCK_INITIALIZER { 0 }
#define __SPIN_LOCK_INITIALIZER     { { 0 } }
#define DEFINE_SPINLOCK(x)  spinlock_t x = __SPIN_LOCK_INITIALIZER
#define DEFINE_RAW_SPINLOCK(x) raw_spinlock_t x = __RAW_SPIN_LOCK_INITIALIZER

static inline void spin_lock_init(spinlock_t *l) { l->rlock.lock = 0; }
static inline void raw_spin_lock_init(raw_spinlock_t *l) { l->lock = 0; }

/* UP cooperative bring-up: locks are NO-OPS. loadAllKexts (where the unmodified DRM driver
 * probes) runs single-threaded with no preemption, and the cooperative vq pump re-enters the
 * driver (a wait_event spin calls vt_interrupt -> the vq callback) WHILE a control-queue lock
 * is held — a real spinlock would self-deadlock there, and DRM's nested modeset locking would
 * deadlock too. No-op acquire is the correct UP model (rwlocks below are already no-ops). */
static inline void __lk_acquire(volatile int *l) { (void)l; }
static inline void __lk_release(volatile int *l) { (void)l; }
static inline int  __lk_try(volatile int *l) { (void)l; return 1; }

static inline void spin_lock(spinlock_t *l)   { __lk_acquire(&l->rlock.lock); }
static inline void spin_unlock(spinlock_t *l) { __lk_release(&l->rlock.lock); }
static inline int  spin_trylock(spinlock_t *l) { return __lk_try(&l->rlock.lock); }
static inline void spin_lock_bh(spinlock_t *l)   { __lk_acquire(&l->rlock.lock); }
static inline void spin_unlock_bh(spinlock_t *l) { __lk_release(&l->rlock.lock); }
static inline int  spin_is_locked(spinlock_t *l) { return l->rlock.lock; }

/* Save RFLAGS + CLI (kext); no-op on the host doctest build (can't/needn't touch RFLAGS there). */
#ifdef NANOS_HOST_TEST
static inline unsigned long __lkpi_irq_save(void) { return 0; }
static inline void __lkpi_irq_restore(unsigned long f) { (void)f; }
#else
static inline unsigned long __lkpi_irq_save(void) {
	unsigned long f;
	__asm__ __volatile__("pushfq; popq %0; cli" : "=r"(f) : : "memory");
	return f;
}
static inline void __lkpi_irq_restore(unsigned long f) {
	__asm__ __volatile__("pushq %0; popfq" : : "r"(f) : "memory", "cc");
}
#endif

/* Plain _irq stays a no-op: only the balanced _irqsave/_irqrestore pair disables IRQs, so we never
 * leak a CLI (spin_unlock_irq has no matching saved-flags to restore). */
static inline void spin_lock_irq(spinlock_t *l)   { __lk_acquire(&l->rlock.lock); }
static inline void spin_unlock_irq(spinlock_t *l) { __lk_release(&l->rlock.lock); }
static inline int  spin_trylock_irq(spinlock_t *l) { return __lk_try(&l->rlock.lock); }
#define spin_trylock_irqsave(l, flags) ({ (flags) = __lkpi_irq_save(); int __ok = spin_trylock(l); if(!__ok) __lkpi_irq_restore(flags); __ok; })

#define spin_lock_irqsave(l, flags)      do { (flags) = __lkpi_irq_save(); spin_lock(l); } while (0)
#define spin_unlock_irqrestore(l, flags) do { spin_unlock(l); __lkpi_irq_restore(flags); } while (0)
/* nested variant: lockdep subclass is meaningless with lockdep off — same lock, same save/restore
 * (i915_sw_fence takes the child fence's lock nested under the parent's). */
#define spin_lock_irqsave_nested(l, flags, subclass) spin_lock_irqsave(l, flags)
#define spin_lock_nested(l, subclass)                spin_lock(l)

static inline void raw_spin_lock(raw_spinlock_t *l)   { __lk_acquire(&l->lock); }
static inline void raw_spin_unlock(raw_spinlock_t *l) { __lk_release(&l->lock); }
#define raw_spin_lock_irqsave(l, flags)      do { (flags) = __lkpi_irq_save(); raw_spin_lock(l); } while (0)
#define raw_spin_unlock_irqrestore(l, flags) do { raw_spin_unlock(l); __lkpi_irq_restore(flags); } while (0)

/* assert helpers used by some Linux code */
#define assert_spin_locked(l) do {} while (0)
#define lockdep_assert_held(l) do {} while (0)

#endif /* _LINUXKPI_LINUX_SPINLOCK_H */

/* rwlock — degenerate to a plain spinlock (single-threaded bring-up) */
#ifndef _LKPI_RWLOCK_DEFINED
#define _LKPI_RWLOCK_DEFINED
typedef struct { volatile int lock; } rwlock_t__unused_;
#define rwlock_init(l)   do { (l)->lock = 0; } while (0)
#define read_lock(l)     do { (void)(l); } while (0)
#define read_unlock(l)   do { (void)(l); } while (0)
#define write_lock(l)    do { (void)(l); } while (0)
#define write_unlock(l)  do { (void)(l); } while (0)
#define read_lock_irqsave(l, f)  do { (f) = 0; (void)(l); } while (0)
#define read_unlock_irqrestore(l, f) do { (void)(f); (void)(l); } while (0)
#define write_lock_irqsave(l, f) do { (f) = 0; (void)(l); } while (0)
#define write_unlock_irqrestore(l, f) do { (void)(f); (void)(l); } while (0)
#endif

#ifndef _LKPI_LOCKDEP_X
#define _LKPI_LOCKDEP_X
#define lockdep_assert_held_once(l) do{}while(0)
#define lockdep_assert_none_held_once() do{}while(0)
#define lockdep_assert_not_held(l) do{}while(0)
#endif

#ifndef _LKPI_LOCKDEP_X2
#define _LKPI_LOCKDEP_X2
struct lockdep_map { int x; };
#define lockdep_assert_once(c) do{}while(0)
#define lockdep_is_held(l) 1
#define lockdep_init_map(m,n,k,s) do{}while(0)
#define lock_acquire_shared_recursive(...) do{}while(0)
#define lock_release(m,i) do{}while(0)
#define lock_acquire(m,a,b,c,d,e,f) do{}while(0)
#define mutex_acquire(m,a,b,c) do{}while(0)
#define mutex_release(m,c) do{}while(0)
#endif
