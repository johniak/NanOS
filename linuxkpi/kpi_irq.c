/*
 * linuxkpi/kpi_irq.c — request_irq / request_threaded_irq / free_irq over the kernel's MSI
 * facility (knx_register_msi from kexports.def).
 *
 * Model: a small descriptor table. lkpi_irq_bind_msi() reserves a slot, hands its index (offset by
 * LKPI_IRQ_BASE) back as the "irq number", and points the kernel MSI trampoline at this file's
 * dispatch. A driver then request_irq()s that number to install its handler — exactly the Linux
 * ordering (pci_alloc_irq_vectors → request_irq). When the device raises its MSI, the kernel
 * trampoline calls knx's registered handler → lkpi_irq_dispatch() → the driver's handler.
 *
 * Interim (until Task 3's async workqueues): request_threaded_irq's thread_fn runs INLINE right
 * after the hard handler returns IRQ_WAKE_THREAD. Every device we bring up here (virtio-gpu) uses a
 * hard-only handler, so this is correct; Task 3 moves thread_fn onto a dedicated irq thread.
 */
#include <linux/interrupt.h>
#include <linux/printk.h>    /* bring-up marker tees to the persistent i915 log */
#include "lkpi_knx.h"

#define LKPI_IRQ_BASE  32          /* shim irq numbers start here (avoid legacy GSI/PIC confusion) */
#define LKPI_IRQ_MAX   16          /* enough for the handful of MSI devices we bring up */

struct lkpi_irq_desc {
	irq_handler_t handler;
	irq_handler_t thread_fn;
	void         *dev;
	unsigned      bound;           /* 1 = an MSI is bound to this slot */
	unsigned long fires;
};

static struct lkpi_irq_desc g_irq[LKPI_IRQ_MAX];
static unsigned long        g_total_fires;

/* Nonzero while an i915 handler / thread_fn is running inline in interrupt context (below). The
 * printk persistent tee reads this: a drm_err logged from here must be RAM-buffered rather than
 * written straight to the USB log, because knx_file_append takes the IRQ-enabled, non-recursive
 * g_xhciLock and an MSI re-entering it mid file-append self-deadlocks. Bumped around the handler
 * calls, not the whole dispatch, so the pre-handler bookkeeping still logs normally. */
volatile int lkpi_in_irq;

/* Log-once the first time any tasklet callback runs (see <linux/interrupt.h>). Non-inline so "once"
 * is a single global flag, not one-per-translation-unit. */
void lkpi_tasklet_first_marker(void) {
	static int once;
	if (once) return;
	once = 1;
	printk("lkpi: FIRST tasklet exec — a tasklet body ran (execlists submission path)\n");
}

/* ---- tasklet body + deferred (softirq-like) drain (P6/Task 3) ------------------------------
 * __lkpi_tasklet_exec runs the body under the atomic RUN/SCHED guard (semantics documented in
 * <linux/interrupt.h>). tasklet_schedule() from thread context calls it inline; from interrupt
 * context it calls lkpi_tasklet_enqueue() instead, and lkpi_tasklet_drain() runs the body later in
 * thread context (the wait pump / drm-node ioctl). This keeps the execlists submission + fence-signal
 * chain OFF the IRQ stack and out of IRQ-enabled-lock re-entry (g_xhciLock via the log tee). */
void __lkpi_tasklet_exec(struct tasklet_struct *t) {
	if (!t)
		return;
	for (;;) {
		if (!tasklet_trylock(t)) {                    /* already running: defer to the owner */
			__atomic_fetch_or(&t->state, 1UL << TASKLET_STATE_SCHED, __ATOMIC_RELEASE);
			return;
		}
		do {
			__atomic_fetch_and(&t->state, ~(1UL << TASKLET_STATE_SCHED), __ATOMIC_ACQ_REL);
			if (t->count) {                           /* disabled: stay pending */
				__atomic_fetch_or(&t->state, 1UL << TASKLET_STATE_SCHED, __ATOMIC_RELEASE);
				break;
			}
			lkpi_tasklet_first_marker();
			if (t->use_callback) { if (t->callback) t->callback(t); }
			else                 { if (t->func) t->func(t->data); }
		} while (t->state & (1UL << TASKLET_STATE_SCHED));
		tasklet_unlock(t);
		/* Re-check after unlock: a schedule that landed after the while-check but before RUN cleared
		 * would trylock-fail and set SCHED with nothing left to run it — a dropped execlists
		 * submission => engine stall. Re-claim; the common path returns on the first pass. */
		if (t->count || !(t->state & (1UL << TASKLET_STATE_SCHED)))
			return;
	}
}

#ifndef LKPI_TASKLET_INLINE
static struct tasklet_struct *g_tl_head, *g_tl_tail;  /* pending list, linked via t->next */
static volatile int g_tl_spin;                        /* cli + test-and-set guard for the list + QUEUED */
static volatile int g_tl_draining;                    /* re-entrancy guard (a body may enter a wait) */

static unsigned long tl_lock(void) {
	unsigned long fl;
#ifndef NANOS_HOST_TEST
	__asm__ __volatile__("pushfq; popq %0; cli" : "=r"(fl) : : "memory");
#else
	fl = 0;
#endif
	while (__atomic_test_and_set(&g_tl_spin, __ATOMIC_ACQUIRE)) {
#ifndef NANOS_HOST_TEST
		__asm__ __volatile__("pause");   /* `pause` is x86-only; the ARM host doctest gets a barrier */
#else
		__asm__ __volatile__("" ::: "memory");
#endif
	}
	return fl;
}
static void tl_unlock(unsigned long fl) {
	__atomic_clear(&g_tl_spin, __ATOMIC_RELEASE);
#ifndef NANOS_HOST_TEST
	__asm__ __volatile__("pushq %0; popfq" : : "r"(fl) : "memory", "cc");
#else
	(void)fl;
#endif
}

/* Interrupt context: mark SCHED and put the tasklet on the pending list (once). The QUEUED bit dedups
 * repeated kicks before the drain gets to it. */
void lkpi_tasklet_enqueue(struct tasklet_struct *t) {
	unsigned long fl;
	if (!t)
		return;
	__atomic_fetch_or(&t->state, 1UL << TASKLET_STATE_SCHED, __ATOMIC_RELEASE);
	fl = tl_lock();
	if (!(t->state & (1UL << TASKLET_STATE_QUEUED))) {
		t->state |= (1UL << TASKLET_STATE_QUEUED);
		t->next = 0;
		if (g_tl_tail) g_tl_tail->next = t; else g_tl_head = t;
		g_tl_tail = t;
	}
	tl_unlock(fl);
}

/* Thread context: run every pending tasklet body. Re-entrancy-guarded (a body that itself enters a
 * cooperative wait -> pump -> drain must not re-drain the same list). */
void lkpi_tasklet_drain(void) {
	if (g_tl_draining)
		return;
	g_tl_draining = 1;
	for (;;) {
		struct tasklet_struct *t;
		unsigned long fl = tl_lock();
		t = g_tl_head;
		if (t) {
			g_tl_head = t->next;
			if (!g_tl_head) g_tl_tail = 0;
			t->next = 0;
			t->state &= ~(1UL << TASKLET_STATE_QUEUED);
		}
		tl_unlock(fl);
		if (!t)
			break;
		__lkpi_tasklet_exec(t);
	}
	g_tl_draining = 0;
}

/* Pulse telemetry (i915_entry.c): pending-list non-empty + draining flag. A pending list that
 * stays non-empty across pulse lines while the owner pumps (drain runs every pump turn) would
 * convict a stuck drain guard / RUN bit; torn reads harmless. */
int lkpi_tasklet_stat(int *draining)
{
	if (draining)
		*draining = g_tl_draining;
	return g_tl_head != 0;
}
#else
void lkpi_tasklet_enqueue(struct tasklet_struct *t) { __lkpi_tasklet_exec(t); }
void lkpi_tasklet_drain(void) { }
int lkpi_tasklet_stat(int *draining) { if (draining) *draining = 0; return 0; }
#endif

static struct lkpi_irq_desc *desc_of(int irq) {
	int i = irq - LKPI_IRQ_BASE;
	if (i < 0 || i >= LKPI_IRQ_MAX)
		return 0;
	return &g_irq[i];
}

/* knx_log takes a string; build "<pfx><n><sfx>" into a small stack buffer. */
static void lkpi_log_num(const char *pfx, unsigned n, const char *sfx) {
	char buf[64];
	int i = 0;
	for (const char *p = pfx; *p && i < 40; p++) buf[i++] = *p;
	char num[12];
	int j = 0;
	if (n == 0) num[j++] = '0';
	while (n && j < 11) { num[j++] = (char)('0' + n % 10); n /= 10; }
	while (j > 0 && i < 52) buf[i++] = num[--j];
	for (const char *p = sfx; *p && i < 63; p++) buf[i++] = *p;
	buf[i] = 0;
	knx_log(buf);
}

int request_irq(unsigned int irq, irq_handler_t h, unsigned long flags,
                const char *name, void *dev) {
	(void)flags; (void)name;
	struct lkpi_irq_desc *d = desc_of((int)irq);
	if (!d)
		return -1;                 /* invalid irq number (Linux: -EINVAL) */
	d->handler   = h;
	d->thread_fn = 0;
	d->dev       = dev;
	return 0;
}

int request_threaded_irq(unsigned int irq, irq_handler_t h, irq_handler_t thread_fn,
                         unsigned long flags, const char *name, void *dev) {
	(void)flags; (void)name;
	struct lkpi_irq_desc *d = desc_of((int)irq);
	if (!d)
		return -1;
	d->handler   = h;              /* may be NULL: threaded-only (default primary wakes the thread) */
	d->thread_fn = thread_fn;
	d->dev       = dev;
	return 0;
}

const void *free_irq(unsigned int irq, void *dev) {
	struct lkpi_irq_desc *d = desc_of((int)irq);
	if (!d)
		return 0;
	(void)dev;
	/* We cannot un-route the MSI (no knx unregister export), but clearing the handlers makes any
	 * further dispatch a no-op — the used-ring poll remains the harvester. Idempotent: a double
	 * free_irq just clears an already-clear slot. */
	d->handler   = 0;
	d->thread_fn = 0;
	d->dev       = 0;
	return 0;
}

void lkpi_irq_dispatch(int irq) {
	struct lkpi_irq_desc *d = desc_of(irq);
	if (!d)
		return;
	if (!d->handler && !d->thread_fn)
		return;                    /* no handler installed yet (spurious early MSI) — ignore */
	d->fires++;
	if (++g_total_fires == 1) {
		knx_log("lkpi: irq fired>0\n");   /* smoke assertion: an MSI reached a request_irq handler */
		printk("lkpi: FIRST irq dispatch (irq %d) — a GT/display MSI was delivered (latch-only)\n", irq);
	}
	/* LATCH-ONLY: the hard-IRQ frame runs NO driver code. Running gen8_irq_handler here executed
	 * the whole display/GT bottom half in interrupt context on whatever core the fixed MSI vector
	 * targets — CONCURRENTLY with the nwm thread's i915 section on another core, under the shim's
	 * no-op locks (every CLI-based exclusion in the shim is per-core). That cross-core window is
	 * the Dell GL-desktop freeze class (see the gate note in lkpi_knx.h). The handlers now run
	 * exclusively via lkpi_irq_poll — the pump harvest — in thread context under the gate; the
	 * poll reads the device's own IIR/CSB registers, so nothing is lost by not running here, the
	 * completion is simply picked up by the next pump turn (every wait pumps; the ktimers daemon
	 * pumps the harvest every ~4 ms for the nobody-is-waiting case). MSIs are edge-triggered and
	 * the IIR bits stay latched in the device until the harvest reads them. */
}

/* The kernel MSI trampoline (knx_register_msi's handler) forwards here with ctx = the irq number. */
static void lkpi_msi_trampoline(void *ctx) {
	lkpi_irq_dispatch((int)(long)ctx);
}

/* Poll-mode harvest of every bound irq. The cooperative wait pump (lkpi_wait_pump) calls this so a
 * fence completed by a device MSI still makes progress when that MSI cannot be delivered right now:
 * the awaited wait runs under spin_lock_irqsave (interrupts CLI'd) or an edge was lost. Each hard
 * handler reads its own interrupt-identity/CSB registers; when nothing is pending it returns IRQ_NONE
 * after a couple of MMIO reads, so idle polling is cheap. Reentrancy-guarded: a handler that itself
 * enters a wait (and thus the pump) must not re-poll and re-run itself. Does NOT bump the fire
 * counters — this is a poll, not a delivered interrupt. */
static volatile int g_irq_polling;
void lkpi_irq_poll(void) {
	unsigned long fl = 0;
	if (g_irq_polling)
		return;
	g_irq_polling = 1;
	lkpi_in_irq++;                 /* handlers run below are interrupt context too — tee must RAM-buffer */
	/* Run the harvest with interrupts DISABLED — a real device MSI must not be delivered in the
	 * MIDDLE of a poll-driven handler run. If it were, the driver's hard handler (and the execlists
	 * submission tasklet it kicks) would re-enter on top of the poll's copy: the tasklet RUN/SCHED
	 * bits are a non-atomic read-modify-write, so a nested interrupt landing mid-update loses a bit
	 * and lets the tasklet run twice — double-completing a request and freeing its i915_sw_fence
	 * under the first completion, so the second call jumps through a freed fence->fn (observed on the
	 * Dell as a #GP at __i915_sw_fence_complete). Masking makes the poll atomic w.r.t. real delivery,
	 * exactly the context a real hard IRQ handler already runs in (IF=0). */
#ifndef NANOS_HOST_TEST
	__asm__ __volatile__("pushfq; popq %0; cli" : "=r"(fl) : : "memory");
#endif
	for (int i = 0; i < LKPI_IRQ_MAX; i++) {
		struct lkpi_irq_desc *d = &g_irq[i];
		irqreturn_t r;
		if (!d->bound || (!d->handler && !d->thread_fn))
			continue;
		/* Re-assert CLI before EACH handler: a prior handler may have run code that unconditionally
		 * re-enables interrupts (execlists_dequeue_irq's local_irq_enable, or now a real
		 * spin_unlock_irq), which would leave IF=1 for the next handler and let a real MSI nest on
		 * top of the poll. gen8_irq_handler zeroes MASTER_IRQ first, so a nested real MSI reads
		 * IIR=0 and exits spurious — but only if we keep the mask tight per-iteration. */
#ifndef NANOS_HOST_TEST
		__asm__ __volatile__("cli" ::: "memory");
#endif
		r = IRQ_WAKE_THREAD;
		if (d->handler)
			r = d->handler(LKPI_IRQ_BASE + i, d->dev);
		if (r == IRQ_WAKE_THREAD && d->thread_fn)
			d->thread_fn(LKPI_IRQ_BASE + i, d->dev);
	}
#ifndef NANOS_HOST_TEST
	__asm__ __volatile__("pushq %0; popfq" : : "r"(fl) : "memory", "cc");
#endif
	lkpi_in_irq--;
	g_irq_polling = 0;
}

#ifdef NANOS_HOST_TEST
/* Host doctest hook: mark a slot bound so lkpi_irq_poll harvests it. On the target `bound` is set
 * by lkpi_irq_bind_msi (a real MSI route); the host has none (knx_register_msi stubs to -1). */
void lkpi_irq_test_bind(int irq) {
	struct lkpi_irq_desc *d = desc_of(irq);
	if (d)
		d->bound = 1;
}
#endif

/* kpi_fence.c: register lkpi_irq_poll as a wait-pump source. Declared here (plain, like lkpi_wait_pump)
 * to avoid a header include cycle; wired on the first MSI bind so any MSI driver gets pump-harvested. */
void lkpi_set_irq_poll(void (*fn)(void));

int lkpi_irq_bind_msi(unsigned bus, unsigned dev, unsigned func) {
	int slot = -1;
	for (int i = 0; i < LKPI_IRQ_MAX; i++) {
		if (!g_irq[i].bound && !g_irq[i].handler) { slot = i; break; }
	}
	if (slot < 0)
		return -1;
	int irq = LKPI_IRQ_BASE + slot;
	g_irq[slot].bound = 1;
	lkpi_set_irq_poll(lkpi_irq_poll);   /* enable pump-driven irq harvest (idempotent across binds) */
	if (knx_register_msi((unsigned char)bus, (unsigned char)dev, (unsigned char)func,
	                     lkpi_msi_trampoline, (void *)(long)irq) < 0) {
		g_irq[slot].bound = 0;
		return -1;                 /* no MSI/MSI-X on this function — caller stays poll/INTx */
	}
	lkpi_log_num("lkpi: irq ", (unsigned)irq, " bound (msi)\n");
	return irq;
}
