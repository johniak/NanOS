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

struct rcu_head;
#ifdef NANOS_HOST_TEST
/* Host doctest: no scheduler, no concurrency — inline callbacks stay correct there. */
#define synchronize_rcu()           smp_mb()
#define synchronize_rcu_expedited() smp_mb()
#define call_rcu(head, func)        ((func)(head))
#define rcu_barrier()               synchronize_rcu()
#else
void knx_rcu_synchronize(void);   /* kexports.def; real grace period (Scheduler::rcuSynchronize) */
#define synchronize_rcu()           knx_rcu_synchronize()
#define synchronize_rcu_expedited() knx_rcu_synchronize()
/* THE QUARANTINE (kpi_rcu.c): call_rcu enqueues; a dedicated drainer thread runs the
 * callbacks only after a real grace period. Inline invocation was an SMP use-after-free
 * window on every rcu-freed object (intel_context/timeline/vma_resource churn). */
void lkpi_call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *));
void lkpi_kfree_rcu_off(struct rcu_head *head, unsigned long off);
void lkpi_rcu_drain(void);
#define call_rcu(head, func)   lkpi_call_rcu((head), (func))
/* rcu_barrier promises every queued callback has RUN — drain synchronously (teardown
 * paths; the caller is a schedulable thread). */
#define rcu_barrier()          lkpi_rcu_drain()
#endif
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

#ifdef __cplusplus
extern "C" void kfree(const void *);
#else
void kfree(const void *);
#endif
#ifdef NANOS_HOST_TEST
#define kfree_rcu(ptr, rhf)      kfree(ptr)
#define kfree_rcu_mightsleep(ptr) kfree(ptr)
#else
/* kfree_rcu(ptr, rcu_member): the upstream offset encoding — a "func" below 4096 is the
 * rcu_head's offset inside its container; the drain kfrees (head - offset) after the
 * grace period. The mightsleep form has no rcu_head, so it waits a grace period inline. */
#define kfree_rcu(ptr, rhf) \
	lkpi_kfree_rcu_off(&(ptr)->rhf, (unsigned long)__builtin_offsetof(__typeof__(*(ptr)), rhf))
#define kfree_rcu_mightsleep(ptr) do { synchronize_rcu(); kfree(ptr); } while (0)
#endif
/* RCU-head lifetime hooks are debug-only; no-ops here. */
#define init_rcu_head(head)               do { (void)(head); } while (0)
#define init_rcu_head_on_stack(head)      do { (void)(head); } while (0)
#define destroy_rcu_head(head)            do { (void)(head); } while (0)
#define destroy_rcu_head_on_stack(head)   do { (void)(head); } while (0)

#endif /* _LINUXKPI_LINUX_RCUPDATE_H */
