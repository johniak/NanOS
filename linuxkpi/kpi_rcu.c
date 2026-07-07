/*
 * linuxkpi/kpi_rcu.c — deferred call_rcu with a REAL grace period (the quarantine).
 *
 * The old shim ran call_rcu callbacks INLINE (and kfree_rcu freed immediately). That is
 * safe on one CPU (NanOS kernel mode is non-preemptive and RCU readers never sleep), but
 * on SMP it is a live use-after-free window: a reader on CPU A holds an rcu_dereference'd
 * pointer while the updater on CPU B unlinks it and frees it in the same call. The hot
 * users in the vendored tree are exactly the request-churn paths Mesa iris will hammer:
 * intel_context, intel_timeline, i915_vma_resource, intel_gt_buffer_pool, i915_sw_fence.
 *
 * Model: call_rcu enqueues onto a locked pending list; a dedicated kernel thread (spawned
 * post-scheduler, same idiom as the workqueue workers) periodically splices the batch,
 * blocks in knx_rcu_synchronize() — Scheduler::rcuSynchronize returns once every other
 * online CPU has context-switched or gone idle, after which no pre-existing reader can
 * still hold a reference (readers never sleep, are never preempted mid-section) — and
 * only then invokes the callbacks. Blocking happens ONLY on the drainer thread, never in
 * the cooperative wait pump: a pumping GEM_WAIT can spin for seconds without a context
 * switch, so a pump-side synchronize could stall every waiter behind one spinner.
 *
 * kfree_rcu uses the upstream offset encoding: a "func" value below 4096 is the offset of
 * the rcu_head inside its container, and the drain kfrees (head - offset).
 *
 * Pre-scheduler (probe time) callbacks just accumulate: only the boot CPU runs then, so
 * nothing can race, and the first drain after Scheduler::init releases them.
 */
#include <linux/rcupdate.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include "lkpi_knx.h"

#define LKPI_KFREE_RCU_OFFSET_MAX 4096ul

static spinlock_t g_rcu_lock;
static struct rcu_head *g_rcu_pending;      /* LIFO — order does not matter for frees */
static unsigned long g_rcu_queued, g_rcu_drained;

void lkpi_call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *))
{
	unsigned long fl;
	head->func = func;
	spin_lock_irqsave(&g_rcu_lock, fl);
	head->next = g_rcu_pending;
	g_rcu_pending = head;
	g_rcu_queued++;
	spin_unlock_irqrestore(&g_rcu_lock, fl);
}

void lkpi_kfree_rcu_off(struct rcu_head *head, unsigned long off)
{
	lkpi_call_rcu(head, (void (*)(struct rcu_head *))off);
}

static void rcu_run_batch(struct rcu_head *batch)
{
	while (batch) {
		struct rcu_head *next = (struct rcu_head *)batch->next;
		unsigned long f = (unsigned long)batch->func;
		if (f < LKPI_KFREE_RCU_OFFSET_MAX)
			kfree((char *)batch - f);
		else
			batch->func(batch);
		g_rcu_drained++;
		batch = next;
	}
}

/* Splice + grace period + run. Blocks in knx_rcu_synchronize — callers must be on a
 * schedulable thread with interrupts enabled (the drainer thread, or rcu_barrier from a
 * teardown path). */
void lkpi_rcu_drain(void)
{
	struct rcu_head *batch;
	unsigned long fl;
	if (!g_rcu_pending)
		return;
	spin_lock_irqsave(&g_rcu_lock, fl);
	batch = g_rcu_pending;
	g_rcu_pending = 0;
	spin_unlock_irqrestore(&g_rcu_lock, fl);
	if (!batch)
		return;
	knx_rcu_synchronize();
	rcu_run_batch(batch);
}

static void rcu_drainer_body(void *arg)
{
	(void)arg;
	for (;;) {
		knx_thread_msleep(10);
		lkpi_rcu_drain();
	}
}

static void lkpi_rcu_start_drainer(void)
{
	if (!knx_thread_spawn(rcu_drainer_body, 0, "krcu"))
		knx_log("lkpi rcu: drainer spawn FAILED — falling back to rcu_barrier-only drains\n");
	else
		knx_log("lkpi rcu: quarantine drainer up (10 ms cadence, real grace periods)\n");
}

/* Called once from lkpi_wq_init (pre-scheduler): defer the drainer spawn exactly like the
 * workqueue workers. */
void lkpi_rcu_init(void)
{
	spin_lock_init(&g_rcu_lock);
	knx_run_after_scheduler(lkpi_rcu_start_drainer);
}
