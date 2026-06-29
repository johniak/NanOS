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

struct wait_queue_entry { void *priv; struct list_head entry; };
typedef struct wait_queue_entry wait_queue_entry_t;

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

/* spin until the condition holds */
#define __wait_event(wq, condition) do { while (!(condition)) __asm__ __volatile__("pause"); } while (0)

#define wait_event(wq, condition)                __wait_event(wq, condition)
#define wait_event_interruptible(wq, condition)  ({ __wait_event(wq, condition); 0; })
#define wait_event_killable(wq, condition)       ({ __wait_event(wq, condition); 0; })

/* timeout variants spin then return remaining ticks (>0); never time out in bring-up */
#define wait_event_timeout(wq, condition, timeout) \
	({ __wait_event(wq, condition); (timeout) ? (timeout) : 1; })
#define wait_event_interruptible_timeout(wq, condition, timeout) \
	({ __wait_event(wq, condition); (timeout) ? (long)(timeout) : 1L; })

#define might_sleep() do {} while (0)

#endif /* _LINUXKPI_LINUX_WAIT_H */

#ifndef _LKPI_WAIT_CMPL
#define _LKPI_WAIT_CMPL
#include <linux/completion.h>
static inline long wait_for_completion_interruptible_timeout(struct completion *x, unsigned long t){ wait_for_completion(x); return t?(long)t:1; }
static inline long wait_for_completion_killable_timeout(struct completion *x, unsigned long t){ wait_for_completion(x); return t?(long)t:1; }
#endif
