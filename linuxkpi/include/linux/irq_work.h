/* linuxkpi/include/linux/irq_work.h — i915 breadcrumbs defer signalling via irq_work. NanOS has no
 * hardirq work queue; the cooperative model runs the callback inline on queue (as call_rcu does). */
#ifndef _LINUXKPI_LINUX_IRQ_WORK_H
#define _LINUXKPI_LINUX_IRQ_WORK_H
#include <linux/types.h>
#include <linux/llist.h>
/* mirrors Linux's __call_single_node: i915 links pending irq_work via work.node.llist. */
struct __call_single_node { struct llist_node llist; };
struct irq_work { struct __call_single_node node; void (*func)(struct irq_work *); };
#define IRQ_WORK_INIT(_f)          { .func = (_f) }
#define DEFINE_IRQ_WORK(n, _f)     struct irq_work n = IRQ_WORK_INIT(_f)
static inline void init_irq_work(struct irq_work *w, void (*f)(struct irq_work *)) { w->func = f; }
static inline bool irq_work_queue(struct irq_work *w) { if (w && w->func) w->func(w); return true; }
static inline bool irq_work_queue_on(struct irq_work *w, int cpu) { (void)cpu; return irq_work_queue(w); }
static inline void irq_work_sync(struct irq_work *w) { (void)w; }
static inline bool irq_work_pending(struct irq_work *w) { (void)w; return false; }
#endif
