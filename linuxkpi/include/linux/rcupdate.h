/*
 * linuxkpi/include/linux/rcupdate.h — RCU for the shim.
 *
 * Read side (rcu_read_lock/unlock) is a no-op, and this is CORRECT, not a stopgap: NanOS uses
 * deferred preemption (docs/en/scheduler.md) — a task in kernel mode is never switched out except
 * at a voluntary schedule(), and every LinuxKPI/i915 code path (including RCU read-side sections)
 * runs in kernel mode. An RCU reader contains no schedule() (readers must not sleep), so it can
 * never be preempted mid-critical-section. Disabling preemption is therefore redundant.
 *
 * Update side (synchronize_rcu) is a REAL grace period now that SMP is live: with multiple CPUs an
 * updater on one CPU must wait for pre-existing readers on the others to finish. Given the read-side
 * invariant above, a CPU that has context-switched (or gone idle) since the call began holds no such
 * reader — so knx_rcu_synchronize (Scheduler::rcuSynchronize) blocks until every other online CPU
 * has done so. On UP it is a barrier (the caller is the only CPU). Under the host doctest harness
 * there is no scheduler, so it degrades to a memory barrier.
 */
#ifndef _LINUXKPI_LINUX_RCUPDATE_H
#define _LINUXKPI_LINUX_RCUPDATE_H

#include <linux/compiler.h>
#include <asm/barrier.h>

#define rcu_read_lock()        do {} while (0)
#define rcu_read_unlock()      do {} while (0)
#define rcu_read_lock_bh()     do {} while (0)
#define rcu_read_unlock_bh()   do {} while (0)

#ifdef NANOS_HOST_TEST
#define synchronize_rcu()           smp_mb()
#define synchronize_rcu_expedited() smp_mb()
#else
void knx_rcu_synchronize(void);   /* kexports.def; real grace period (Scheduler::rcuSynchronize) */
#define synchronize_rcu()           knx_rcu_synchronize()
#define synchronize_rcu_expedited() knx_rcu_synchronize()
#endif
#define call_rcu(head, func)   ((func)(head))
#define rcu_barrier()          synchronize_rcu()
#define cond_synchronize_rcu(oldstate)  synchronize_rcu()
#define get_state_synchronize_rcu()     (0UL)
#define start_poll_synchronize_rcu()    (0UL)
#define poll_state_synchronize_rcu(s)   (true)

#define rcu_dereference(p)             READ_ONCE(p)
#define rcu_dereference_protected(p, c) (p)
#define rcu_dereference_raw(p)         READ_ONCE(p)
#define rcu_access_pointer(p)          READ_ONCE(p)
#define rcu_assign_pointer(p, v)       smp_store_release(&(p), (v))
#define RCU_INIT_POINTER(p, v)         do { (p) = (v); } while (0)
#define rcu_replace_pointer(rp, p, c)  ({ __typeof__(p) __old = (rp); rcu_assign_pointer((rp), (p)); __old; })
/* unrcu_pointer: strip the __rcu annotation, returning the raw pointer. No sparse/annotation layer
 * here, so it is the identity read. dma-fence-chain walks its chain with it. */
#define unrcu_pointer(p)               (p)
/* RCU_INITIALIZER: wrap a pointer as an __rcu initializer value. No annotation layer here, so it is
 * the identity — dma-fence-chain uses it to initialize its rcu-protected prev pointer. */
#define RCU_INITIALIZER(v)             (v)

struct rcu_head { void *next; void (*func)(struct rcu_head *); };

/* kfree_rcu(ptr, rcu_member): with call_rcu running the callback inline (see above), the object
 * can be freed immediately — no reader can still hold it (readers never sleep, are never preempted).
 * Both the 2-arg (offset form) and 1-arg mightsleep form reduce to a plain kfree. */
#ifdef __cplusplus
extern "C" void kfree(const void *);
#else
void kfree(const void *);
#endif
#define kfree_rcu(ptr, rhf)      kfree(ptr)
#define kfree_rcu_mightsleep(ptr) kfree(ptr)
/* RCU-head lifetime hooks are debug-only; no-ops here. */
#define init_rcu_head(head)               do { (void)(head); } while (0)
#define init_rcu_head_on_stack(head)      do { (void)(head); } while (0)
#define destroy_rcu_head(head)            do { (void)(head); } while (0)
#define destroy_rcu_head_on_stack(head)   do { (void)(head); } while (0)

#endif /* _LINUXKPI_LINUX_RCUPDATE_H */
