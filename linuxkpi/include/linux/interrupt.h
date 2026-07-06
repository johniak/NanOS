#ifndef _LKPI_INTERRUPT_H
#define _LKPI_INTERRUPT_H
#include <linux/types.h>
#include <linux/irqreturn.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef irqreturn_t (*irq_handler_t)(int, void*);

#define IRQF_SHARED     0x00000080
#define IRQF_ONESHOT    0x00002000
#define IRQF_NO_AUTOEN  0x00080000

/* Real request_irq / free_irq over the kernel's MSI facility (kpi_irq.c). The `irq` number is one
 * handed out by lkpi_irq_bind_msi() — an index into the shim's irq descriptor table, NOT a GSI.
 * A driver's flow mirrors Linux: bind the device's MSI to get an irq number, then request_irq() it. */
int         request_irq(unsigned int irq, irq_handler_t h, unsigned long flags,
                        const char *name, void *dev);
int         request_threaded_irq(unsigned int irq, irq_handler_t h, irq_handler_t thread_fn,
                                 unsigned long flags, const char *name, void *dev);
const void *free_irq(unsigned int irq, void *dev);

/* Bind a PCI function's MSI/MSI-X (via knx_register_msi) to a fresh shim irq number. Returns the
 * irq number to hand request_irq(), or -1 if the function exposes neither MSI nor MSI-X. */
int  lkpi_irq_bind_msi(unsigned bus, unsigned dev, unsigned func);

/* Deliver `irq` as if its interrupt fired: runs the registered hard handler (and, interim until
 * async workqueues land, its thread_fn inline on IRQ_WAKE_THREAD). Called by the MSI trampoline;
 * also the doctest entry point. */
void lkpi_irq_dispatch(int irq);

/* Bring-up marker: printed ONCE (kpi_irq.c) the first time a tasklet callback actually runs. i915's
 * execlists submission runs from a tasklet, so this positions the first GPU submission in the boot
 * log — the point past which a GT hang means "submission ran but the engine never retired". */
void lkpi_tasklet_first_marker(void);

enum { TASKLET_STATE_SCHED, TASKLET_STATE_RUN };
struct tasklet_struct {
	struct tasklet_struct *next;
	unsigned long state;
	int count;                    /* atomic in Linux; single-threaded bring-up -> plain int */
	bool use_callback;
	union {
		void (*func)(unsigned long data);
		void (*callback)(struct tasklet_struct *t);
	};
	unsigned long data;
};
/* We run tasklets synchronously on schedule (no softirq thread). i915's execlists submission tasklet
 * reschedules ITSELF — from its own body (start_timeslice), from the GT interrupt handler, and via
 * __execlists_kick — so a naive inline run recurses without bound on the first GPU submission:
 * stack overflow -> triple-fault reboot (observed on the Dell during intel_gt_resume). Guard with the
 * upstream RUN/SCHED semantics: tasklet_trylock claims the RUN bit and FAILS while the tasklet is
 * already on the stack, so both __intel_engine_flush_submission's manual run and a nested schedule
 * defer instead of recursing. A schedule that loses the race sets SCHED; the owning run loops until
 * SCHED is clear. tasklet_is_locked (test_bit RUN) and __tasklet_is_enabled (count) stay consistent. */
static inline int tasklet_trylock(struct tasklet_struct *t){
	if (!t) return 1;
	if (t->state & (1UL << TASKLET_STATE_RUN)) return 0;
	t->state |= (1UL << TASKLET_STATE_RUN);
	return 1;
}
static inline void tasklet_unlock(struct tasklet_struct *t){ if (t) t->state &= ~(1UL << TASKLET_STATE_RUN); }
static inline void tasklet_unlock_wait(struct tasklet_struct *t){ (void)t; }
static inline void tasklet_unlock_spin_wait(struct tasklet_struct *t){ (void)t; }
static inline void __lkpi_tasklet_exec(struct tasklet_struct *t){
	if (!t) return;
	for (;;) {
		if (!tasklet_trylock(t)) { t->state |= (1UL << TASKLET_STATE_SCHED); return; }  /* re-entry: defer to owner */
		do {
			t->state &= ~(1UL << TASKLET_STATE_SCHED);
			if (t->count) { t->state |= (1UL << TASKLET_STATE_SCHED); break; }  /* disabled: stay pending */
			lkpi_tasklet_first_marker();   /* log-once: first tasklet body runs (first submission path) */
			if (t->use_callback) { if (t->callback) t->callback(t); }
			else                 { if (t->func) t->func(t->data); }
		} while (t->state & (1UL << TASKLET_STATE_SCHED));
		tasklet_unlock(t);
		/* Close the lost-kick window: a schedule (e.g. from the GT IRQ) that lands AFTER the while-check
		 * saw SCHED clear but BEFORE unlock clears RUN would trylock-fail and set SCHED with no softirq
		 * left to run it — a silently dropped execlists submission => engine stalls (a HANG, not a
		 * reboot). Re-check after unlock and re-claim; the common path returns on the first pass. */
		if (t->count || !(t->state & (1UL << TASKLET_STATE_SCHED))) return;
	}
}
static inline void tasklet_schedule(struct tasklet_struct *t){ __lkpi_tasklet_exec(t); }
static inline void tasklet_hi_schedule(struct tasklet_struct *t){ __lkpi_tasklet_exec(t); }
static inline void tasklet_enable(struct tasklet_struct *t){ (void)t; }
static inline void tasklet_disable(struct tasklet_struct *t){ (void)t; }
static inline void tasklet_disable_nosync(struct tasklet_struct *t){ (void)t; }
static inline void tasklet_kill(struct tasklet_struct *t){ (void)t; }
/* tasklet_setup: modern init form (callback style). */
static inline void tasklet_setup(struct tasklet_struct *t, void (*callback)(struct tasklet_struct *)){ if(!t) return; t->next=0; t->state=0; t->count=0; t->use_callback=true; t->callback=callback; t->data=0; }
/* IRQ quiescence: the shim's IRQ handlers run to completion inline (no threaded/pending IRQ), so
 * synchronize_irq has nothing to wait for. */
static inline void synchronize_irq(unsigned int irq){ (void)irq; }
static inline int  synchronize_hardirq(unsigned int irq){ (void)irq; return 0; }
/* from_tasklet == container_of(callback_arg, type, tasklet_field). */
#define from_tasklet(var, callback_tasklet, tasklet_fieldname) container_of(callback_tasklet, __typeof__(*var), tasklet_fieldname)
/* Bottom-half / softirq disable: cooperative kernel never runs softirqs concurrently -> no-ops. */
static inline void local_bh_disable(void){}
static inline void local_bh_enable(void){}
#ifdef __cplusplus
}
#endif
#endif
