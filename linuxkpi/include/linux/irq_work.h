/* linuxkpi/include/linux/irq_work.h — i915 breadcrumbs defer signalling via irq_work. NanOS has no
 * hardirq work queue; the cooperative model runs the callback inline on queue (as call_rcu does). */
#ifndef _LINUXKPI_LINUX_IRQ_WORK_H
#define _LINUXKPI_LINUX_IRQ_WORK_H
#include <linux/types.h>
#include <linux/llist.h>
/* mirrors Linux's __call_single_node: i915 links pending irq_work via work.node.llist. */
struct __call_single_node { struct llist_node llist; };
/* lkpi_run/lkpi_again (private): re-entrancy guard for the inline-on-queue model below. */
struct irq_work { struct __call_single_node node; void (*func)(struct irq_work *);
                  volatile int lkpi_run, lkpi_again; };
#define IRQ_WORK_INIT(_f)          { .func = (_f) }
#define DEFINE_IRQ_WORK(n, _f)     struct irq_work n = IRQ_WORK_INIT(_f)
static inline void init_irq_work(struct irq_work *w, void (*f)(struct irq_work *)) { w->func = f; w->lkpi_run = 0; w->lkpi_again = 0; }
/* Run the callback inline (NanOS has no hardirq-work queue), but with mainline's iterate-don't-recurse
 * semantics. i915 breadcrumbs re-queue the SAME irq_work from WITHIN its callback (signal_irq_work in
 * gt/intel_breadcrumbs.c re-arms via insert_breadcrumb / arm_irq while it is running, and on real HW
 * kernel-context requests complete in microseconds so this fires constantly) — an unconditional inline
 * call would recurse on the interrupt stack until it overflows. In Linux a queue-while-running just
 * sets PENDING and the handler runs AGAIN after it returns; mirror that: if we are already inside this
 * work, set `again` and let the owning frame loop, so the depth stays 1 no matter how deep the request
 * chain. */
static inline bool irq_work_queue(struct irq_work *w) {
	if (!w || !w->func) return true;
	if (w->lkpi_run) { w->lkpi_again = 1; return true; }
	w->lkpi_run = 1;
	do { w->lkpi_again = 0; w->func(w); } while (w->lkpi_again);
	w->lkpi_run = 0;
	return true;
}
static inline bool irq_work_queue_on(struct irq_work *w, int cpu) { (void)cpu; return irq_work_queue(w); }
static inline void irq_work_sync(struct irq_work *w) { (void)w; }
static inline bool irq_work_pending(struct irq_work *w) { return w && w->lkpi_again; }
#endif
