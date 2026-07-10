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

enum { TASKLET_STATE_SCHED, TASKLET_STATE_RUN, TASKLET_STATE_QUEUED };
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
/* i915's execlists submission tasklet reschedules ITSELF — from its own body (start_timeslice), from
 * the GT interrupt handler, and via __execlists_kick — so a naive inline run recurses without bound on
 * the first GPU submission. Guard with the upstream RUN/SCHED semantics: tasklet_trylock claims the RUN
 * bit and FAILS while the tasklet is already running, so a nested schedule and __intel_engine_flush_
 * submission's manual run defer instead of recursing. The RMW is ATOMIC: under SMP the body must run on
 * exactly one CPU (a double-run frees an i915_sw_fence under the first completion -> #GP in the
 * second), and with the deferred-tasklet path below the drain and an inline run can now race across
 * CPUs. A schedule that loses the race sets SCHED; the owner loops until SCHED is clear. */
static inline int tasklet_trylock(struct tasklet_struct *t){
	if (!t) return 1;
	unsigned long prev = __atomic_fetch_or(&t->state, 1UL << TASKLET_STATE_RUN, __ATOMIC_ACQUIRE);
	return (prev & (1UL << TASKLET_STATE_RUN)) ? 0 : 1;
}
static inline void tasklet_unlock(struct tasklet_struct *t){ if (t) __atomic_fetch_and(&t->state, ~(1UL << TASKLET_STATE_RUN), __ATOMIC_RELEASE); }
static inline void tasklet_unlock_wait(struct tasklet_struct *t){ (void)t; }
static inline void tasklet_unlock_spin_wait(struct tasklet_struct *t){ (void)t; }

/* Run the tasklet body (RUN/SCHED-guarded). Defined out-of-line in kpi_irq.c so the deferred-tasklet
 * drain there can also invoke it. P6/Task 3: tasklet_schedule() reached from INTERRUPT context
 * (lkpi_in_irq!=0 — the GT hard handler / the wait-pump poll harvest) must NOT run the body on the IRQ
 * stack (deep chain + it re-enters IRQ-enabled locks like g_xhciLock via the log tee = the Dell
 * freeze). Linux runs tasklets in softirq, not hardirq: mirror that by ENQUEUEing, and let the
 * cooperative wait pump / drm-node ioctl drain it in thread context. A schedule from THREAD context
 * still runs inline, so callers that expect synchronous submission are unaffected. */
extern volatile int lkpi_in_irq;
void __lkpi_tasklet_exec(struct tasklet_struct *t);
void lkpi_tasklet_enqueue(struct tasklet_struct *t);
void lkpi_tasklet_drain(void);
static inline void tasklet_schedule(struct tasklet_struct *t){ if (lkpi_in_irq) lkpi_tasklet_enqueue(t); else __lkpi_tasklet_exec(t); }
static inline void tasklet_hi_schedule(struct tasklet_struct *t){ if (lkpi_in_irq) lkpi_tasklet_enqueue(t); else __lkpi_tasklet_exec(t); }
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
