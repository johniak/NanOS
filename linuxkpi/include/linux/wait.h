/*
 * linuxkpi/include/linux/wait.h — wait queues for the shim.
 *
 * Bring-up model: wait_event* spins on the condition with a pause hint (cooperative; the
 * waker runs on another thread/IRQ and updates the condition). wake_up is a barrier. This
 * is enough for the DRM/virtio paths that wait on a fence/flag; a true blocking waitqueue
 * can replace it without changing callers.
 */
#ifndef _LINUXKPI_LINUX_WAIT_H
#define _LINUXKPI_LINUX_WAIT_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <asm/barrier.h>

typedef struct wait_queue_head {
	spinlock_t lock;
	struct list_head head;
} wait_queue_head_t;

struct wait_queue_entry;
/* wake callback: return non-zero if the entry was woken (autoremove_wake_function removes it). */
typedef int (*wait_queue_func_t)(struct wait_queue_entry *wq_entry, unsigned mode, int flags, void *key);
struct wait_queue_entry { unsigned int flags; void *private; wait_queue_func_t func; struct list_head entry; };
typedef struct wait_queue_entry wait_queue_entry_t;
int autoremove_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int sync, void *key);
int default_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int sync, void *key);
static inline void init_waitqueue_entry(struct wait_queue_entry *e, void *task){ e->flags=0; e->private=task; e->func=default_wake_function; INIT_LIST_HEAD(&e->entry); }
static inline void init_wait_entry(struct wait_queue_entry *e, int flags){ e->flags=flags; e->private=0; e->func=autoremove_wake_function; INIT_LIST_HEAD(&e->entry); }
/* DEFINE_WAIT declares an on-stack entry whose wake callback auto-removes it from the queue. */
#define DEFINE_WAIT_FUNC(name, function) \
	struct wait_queue_entry name = { .flags = 0, .private = 0, .func = (function), .entry = { &(name).entry, &(name).entry } }
#define DEFINE_WAIT(name) DEFINE_WAIT_FUNC(name, autoremove_wake_function)

#define DECLARE_WAIT_QUEUE_HEAD(name) wait_queue_head_t name = { { {0} }, { &(name).head, &(name).head } }

static inline void init_waitqueue_head(wait_queue_head_t *q) {
	spin_lock_init(&q->lock);
	INIT_LIST_HEAD(&q->head);
}

#define __wake_up(q)  do { smp_mb(); } while (0)
#define wake_up(q)                 __wake_up(q)
#define wake_up_all(q)             __wake_up(q)
#define wake_up_interruptible(q)   __wake_up(q)
#define wake_up_interruptible_all(q) __wake_up(q)
#define wake_up_poll(q, m)         __wake_up(q)

/* spin until the condition holds. lkpi_wait_pump() services any registered poll source
 * (the virtio control/cursor vq) so a driver waiting on vq acks/responses at boot — before
 * the scheduler and device IRQ exist — still makes progress (cooperative polling). */
void lkpi_wait_pump(void);
#define __wait_event(wq, condition) do { while (!(condition)) { lkpi_wait_pump(); __asm__ __volatile__("pause"); } } while (0)

#define wait_event(wq, condition)                __wait_event(wq, condition)
#define wait_event_interruptible(wq, condition)  ({ __wait_event(wq, condition); 0; })
#define wait_event_killable(wq, condition)       ({ __wait_event(wq, condition); 0; })

/* timeout variants: bounded cooperative poll. Spin up to ~`timeout` jiffies of pump cycles
 * (pumping the vq each turn); return remaining ticks (>0) if the condition was met, else 0
 * (timed out). MUST be bounded — virtio_gpu's display-info probe waits here, and an infinite
 * __wait_event would stall the whole boot if a response is lost. */
#define __wait_event_to(wq, condition, timeout) ({ \
		long __t = (long)(timeout); long __n = __t > 0 ? __t : 1; \
		while (!(condition) && __n-- > 0) { lkpi_wait_pump(); __asm__ __volatile__("pause"); } \
		(condition) ? (__n > 0 ? __n : 1L) : 0L; })
#define wait_event_timeout(wq, condition, timeout)               __wait_event_to(wq, condition, timeout)
#define wait_event_interruptible_timeout(wq, condition, timeout) __wait_event_to(wq, condition, timeout)
#define wait_event_killable_timeout(wq, condition, timeout)      __wait_event_to(wq, condition, timeout)

#define might_sleep() do {} while (0)

/* Queue membership. wake_up is a barrier (waiters spin on the condition), so these only maintain the
 * list; the entry's ->func is invoked only if a caller walks the queue itself. */
static inline void add_wait_queue(wait_queue_head_t *q, struct wait_queue_entry *e){ unsigned long f; spin_lock_irqsave(&q->lock,f); list_add(&e->entry,&q->head); spin_unlock_irqrestore(&q->lock,f); }
static inline void add_wait_queue_exclusive(wait_queue_head_t *q, struct wait_queue_entry *e){ unsigned long f; spin_lock_irqsave(&q->lock,f); e->flags|=0x01/*WQ_FLAG_EXCLUSIVE*/; list_add_tail(&e->entry,&q->head); spin_unlock_irqrestore(&q->lock,f); }
static inline void remove_wait_queue(wait_queue_head_t *q, struct wait_queue_entry *e){ unsigned long f; spin_lock_irqsave(&q->lock,f); list_del_init(&e->entry); spin_unlock_irqrestore(&q->lock,f); }
static inline void prepare_to_wait(wait_queue_head_t *q, struct wait_queue_entry *e, int state){ (void)state; unsigned long f; spin_lock_irqsave(&q->lock,f); if(list_empty(&e->entry)) list_add(&e->entry,&q->head); spin_unlock_irqrestore(&q->lock,f); }
static inline void prepare_to_wait_exclusive(wait_queue_head_t *q, struct wait_queue_entry *e, int state){ (void)state; unsigned long f; spin_lock_irqsave(&q->lock,f); e->flags|=0x01; if(list_empty(&e->entry)) list_add_tail(&e->entry,&q->head); spin_unlock_irqrestore(&q->lock,f); }
static inline long prepare_to_wait_event(wait_queue_head_t *q, struct wait_queue_entry *e, int state){ prepare_to_wait(q,e,state); return 0; }
static inline void finish_wait(wait_queue_head_t *q, struct wait_queue_entry *e){ remove_wait_queue(q,e); }
#define WQ_FLAG_EXCLUSIVE 0x01
#define WQ_FLAG_WOKEN     0x02
#define WQ_FLAG_BOOKMARK  0x04
static inline long wait_woken(struct wait_queue_entry *e, unsigned mode, long timeout){ (void)e;(void)mode; return timeout; }
static inline int woken_wake_function(struct wait_queue_entry *e, unsigned mode, int sync, void *key){ (void)e;(void)mode;(void)sync;(void)key; return 1; }

#endif /* _LINUXKPI_LINUX_WAIT_H */

#ifndef _LKPI_WAIT_CMPL
#define _LKPI_WAIT_CMPL
#include <linux/completion.h>
static inline long wait_for_completion_interruptible_timeout(struct completion *x, unsigned long t){ wait_for_completion(x); return t?(long)t:1; }
static inline long wait_for_completion_killable_timeout(struct completion *x, unsigned long t){ wait_for_completion(x); return t?(long)t:1; }
#endif
#ifndef _LKPI_WAIT_POLL
#define _LKPI_WAIT_POLL
#define wake_up_interruptible_poll(q, m) __wake_up(q)
#define wake_up_poll(q, m) __wake_up(q)
#endif
#ifndef _LKPI_WAIT_LOCK_IRQ
#define _LKPI_WAIT_LOCK_IRQ
/* cooperative bring-up: the caller holds `lock`; we drop it around the spin so the
 * condition-setter can take it, then reacquire before returning (as Linux does). */
#define wait_event_lock_irq(wq, condition, lock) do { \
		while (!(condition)) { spin_unlock_irq(&(lock)); __asm__ __volatile__("pause"); spin_lock_irq(&(lock)); } \
	} while (0)
#define wait_event_interruptible_lock_irq(wq, condition, lock) \
	({ wait_event_lock_irq(wq, condition, lock); 0; })
#endif
