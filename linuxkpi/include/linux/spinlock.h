/*
 * linuxkpi/include/linux/spinlock.h — spinlocks for the shim.
 *
 * A test-and-set lock via GCC atomics. NOTE: spin_lock_irqsave currently does NOT disable
 * local interrupts (no knx cli/sti export yet); it is added in P1.3 when the virtio IRQ
 * handler is wired (needed for IRQ-vs-thread mutual exclusion on the vq). Single-threaded
 * bring-up is correct as-is.
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

static inline void __lk_acquire(volatile int *l) {
	while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) { while (*l) __asm__ __volatile__("pause"); }
}
static inline void __lk_release(volatile int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }
static inline int  __lk_try(volatile int *l) { return __atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE) == 0; }

static inline void spin_lock(spinlock_t *l)   { __lk_acquire(&l->rlock.lock); }
static inline void spin_unlock(spinlock_t *l) { __lk_release(&l->rlock.lock); }
static inline int  spin_trylock(spinlock_t *l) { return __lk_try(&l->rlock.lock); }
static inline void spin_lock_bh(spinlock_t *l)   { __lk_acquire(&l->rlock.lock); }
static inline void spin_unlock_bh(spinlock_t *l) { __lk_release(&l->rlock.lock); }
static inline void spin_lock_irq(spinlock_t *l)   { __lk_acquire(&l->rlock.lock); }
static inline void spin_unlock_irq(spinlock_t *l) { __lk_release(&l->rlock.lock); }
static inline int  spin_is_locked(spinlock_t *l) { return l->rlock.lock; }

#define spin_lock_irqsave(l, flags)      do { (flags) = 0; spin_lock(l); } while (0)
#define spin_unlock_irqrestore(l, flags) do { (void)(flags); spin_unlock(l); } while (0)

static inline void raw_spin_lock(raw_spinlock_t *l)   { __lk_acquire(&l->lock); }
static inline void raw_spin_unlock(raw_spinlock_t *l) { __lk_release(&l->lock); }
#define raw_spin_lock_irqsave(l, flags)      do { (flags) = 0; raw_spin_lock(l); } while (0)
#define raw_spin_unlock_irqrestore(l, flags) do { (void)(flags); raw_spin_unlock(l); } while (0)

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
