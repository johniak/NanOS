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

/* Log-once the first time any tasklet callback runs (see <linux/interrupt.h>). Non-inline so "once"
 * is a single global flag, not one-per-translation-unit. */
void lkpi_tasklet_first_marker(void) {
	static int once;
	if (once) return;
	once = 1;
	printk("lkpi: FIRST tasklet exec — a tasklet body ran (execlists submission path)\n");
}

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
		printk("lkpi: FIRST irq dispatch (irq %d) — a GT/display MSI was delivered\n", irq);
	}
	/* An MSI delivered ON TOP of an already-deep i915 call chain runs the handler (and any inline
	 * tasklet it schedules) on the same stack — a candidate for the overflow. Name it before it dies. */
	if (lkpi_stack_deep()) lkpi_deep_report("lkpi_irq_dispatch", __builtin_return_address(0));

	irqreturn_t r = IRQ_WAKE_THREAD;      /* h==NULL means "always wake the thread" (Linux default) */
	if (d->handler)
		r = d->handler(irq, d->dev);
	if (r == IRQ_WAKE_THREAD && d->thread_fn)
		d->thread_fn(irq, d->dev);        /* interim inline; Task 3 moves this to an irq thread */
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
	g_irq_polling = 0;
}

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
