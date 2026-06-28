/*
 * linuxkpi/include/linux/rcupdate.h — RCU for the shim: no real grace periods. The driver
 * runs cooperatively; readers and the updater do not preempt mid-critical-section in the
 * ways RCU guards against here, so read-side markers are plain and synchronize_rcu() is a
 * barrier. (Sufficient for the single-GPU bring-up; revisit if a hot RCU path appears.)
 */
#ifndef _LINUXKPI_LINUX_RCUPDATE_H
#define _LINUXKPI_LINUX_RCUPDATE_H

#include <linux/compiler.h>
#include <asm/barrier.h>

#define rcu_read_lock()        do {} while (0)
#define rcu_read_unlock()      do {} while (0)
#define rcu_read_lock_bh()     do {} while (0)
#define rcu_read_unlock_bh()   do {} while (0)
#define synchronize_rcu()      smp_mb()
#define synchronize_rcu_expedited() smp_mb()
#define call_rcu(head, func)   ((func)(head))
#define rcu_barrier()          smp_mb()

#define rcu_dereference(p)             READ_ONCE(p)
#define rcu_dereference_protected(p, c) (p)
#define rcu_dereference_raw(p)         READ_ONCE(p)
#define rcu_access_pointer(p)          READ_ONCE(p)
#define rcu_assign_pointer(p, v)       smp_store_release(&(p), (v))
#define RCU_INIT_POINTER(p, v)         do { (p) = (v); } while (0)
#define rcu_replace_pointer(rp, p, c)  ({ __typeof__(p) __old = (rp); rcu_assign_pointer((rp), (p)); __old; })

struct rcu_head { void *next; void (*func)(struct rcu_head *); };

#endif /* _LINUXKPI_LINUX_RCUPDATE_H */
