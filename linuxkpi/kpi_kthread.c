/*
 * linuxkpi/kpi_kthread.c — kthreads, ASYNC workqueues and real timers for the shim (i915 plan
 * Task 3). Replaces the synchronous-inline workqueue stubs so i915's display/retire/hangcheck work
 * (which assumes async) can run, without breaking the virtio-gpu bring-up that relied on inline.
 *
 * Model:
 *  - Workqueues each get ONE worker kernel thread (knx_thread_spawn), spawned AFTER the scheduler is
 *    up (kexts load pre-scheduler). Until then — and under a forced LKPI_WQ_INLINE build — queue_work
 *    runs the work inline, exactly as the old stubs did, so the cooperative probe path is unchanged.
 *  - The cooperative wait pump (lkpi_wait_pump) also DRAINS pending work (lkpi_wq_drain), so a
 *    __wait_event that busy-spins expecting a deferred bottom half to run still makes progress even
 *    before/without a worker being scheduled. This is what makes async safe on top of the pump.
 *  - Timers ride a single deadline list scanned by one timer kernel thread; delayed_work arms a timer
 *    that queues the work on expiry. jiffies here are milliseconds (knx_uptime_us()/1000, HZ=1000).
 *
 * Locking: a single real test-and-set lock (kt_lock) guards the workqueue lists + the timer list.
 * The shim's <linux/spinlock.h> is a cooperative no-op, so it cannot serialise a worker against a
 * producer; this lock can (and is SMP-correct for when i915 runs on real cores). It is held only for
 * O(1) list splices — never across a work callback or a yield — so it never deadlocks the model.
 */
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/timer.h>
#include <linux/sched.h>     /* full struct task_struct (kthread handle) */
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/printk.h>    /* bring-up markers tee to the persistent i915 log */
#include "lkpi_knx.h"

/* ---- a genuine tiny spinlock (independent of the no-op shim spinlock) ---- */
/* CPU relax hint: `pause` on the x86 kext target; a bare compiler barrier under the host doctest
 * (which may build for ARM, where `pause` is not a valid mnemonic — and is uncontended there). */
#ifdef NANOS_HOST_TEST
static inline void kt_relax(void) { __asm__ __volatile__("" ::: "memory"); }
#else
static inline void kt_relax(void) { __asm__ __volatile__("pause"); }
#endif
typedef struct { volatile int v; } kt_lock_t;
static inline void kt_lock(kt_lock_t *l)   { while (__sync_lock_test_and_set(&l->v, 1)) kt_relax(); }
static inline void kt_unlock(kt_lock_t *l) { __sync_lock_release(&l->v); }

static kt_lock_t g_lock;                 /* guards all workqueue lists + pending flags */
static kt_lock_t g_tlock;                /* guards the timer list */
static volatile int g_wq_async = 0;      /* 0 = run inline (pre-scheduler / LKPI_WQ_INLINE) */

static unsigned long jiffies_now(void) { return (unsigned long)(knx_uptime_us() / 1000ull); }

/* ======================= workqueues ======================= */

#define WQ_MAX 16
struct workqueue_struct {
	struct list_head pending;                 /* work_struct.entry list */
	struct work_struct * volatile current_w;  /* work currently running (for flush/cancel) */
	void        *worker;                      /* knx thread handle (0 until started) */
	int          used;
	const char  *name;
};
static struct workqueue_struct g_wqs[WQ_MAX];
static int g_nwq = 0;

/* the well-known system queues (all real, each its own worker) */
struct workqueue_struct *system_wq, *system_unbound_wq, *system_long_wq, *system_highpri_wq;

static struct workqueue_struct *wq_new(const char *name) {
	if (g_nwq >= WQ_MAX) {
		knx_log("lkpi: workqueue cap reached — reusing system_wq\n");
		return system_wq ? system_wq : &g_wqs[0];
	}
	struct workqueue_struct *q = &g_wqs[g_nwq++];
	INIT_LIST_HEAD(&q->pending);
	q->current_w = 0;
	q->worker = 0;
	q->used = 1;
	q->name = name;
	return q;
}

/* run one pending item from q (locked pop, unlocked run). Returns 1 if one ran. */
static int wq_run_one(struct workqueue_struct *q) {
	struct work_struct *w = 0;
	kt_lock(&g_lock);
	if (!list_empty(&q->pending)) {
		w = list_first_entry(&q->pending, struct work_struct, entry);
		list_del_init(&w->entry);
		w->pending = 0;
		q->current_w = w;
	}
	kt_unlock(&g_lock);
	if (!w)
		return 0;
	if (w->func)
		w->func(w);
	q->current_w = 0;
	return 1;
}

static void wq_enqueue(struct workqueue_struct *q, struct work_struct *w) {
	kt_lock(&g_lock);
	if (!w->pending) {
		w->pending = 1;
		list_add_tail(&w->entry, &q->pending);
	}
	kt_unlock(&g_lock);
}

bool queue_work(struct workqueue_struct *q, struct work_struct *w) {
	if (!q) q = system_wq;
	if (!q) { if (w->func) w->func(w); return true; }   /* before lkpi_wq_init: inline */
	if (!g_wq_async) { if (w->func) w->func(w); return true; }
	wq_enqueue(q, w);
	return true;
}
bool schedule_work(struct work_struct *w) { return queue_work(system_wq, w); }

static int work_is_running(struct work_struct *w) {
	for (int i = 0; i < g_nwq; i++)
		if (g_wqs[i].current_w == w)
			return 1;
	return 0;
}

/* Drain every workqueue once (bounded). Called by the cooperative wait pump AND the workers. */
void lkpi_wq_drain(void) {
	for (int i = 0; i < g_nwq; i++) {
		int guard = 4096;
		while (wq_run_one(&g_wqs[i]) && --guard > 0) {}
	}
}

void flush_work(struct work_struct *w) {
	int guard = 1 << 20;
	while ((w->pending || work_is_running(w)) && --guard > 0) {
		lkpi_wq_drain();
		knx_thread_yield();
	}
}

/* Flush a delayed_work: if its timer is still armed, fire the work immediately (cancel the delay),
 * then wait for the underlying work to complete. Returns true if work was pending. */
bool flush_delayed_work(struct delayed_work *dw) {
	bool was_pending = false;
	if (!dw) return false;
	if (dw->timer.lkpi_linked) {           /* delay not yet elapsed: run now */
		del_timer(&dw->timer);
		if (dw->wq) queue_work(dw->wq, &dw->work);
		else        schedule_work(&dw->work);
		was_pending = true;
	}
	if (dw->work.pending || work_is_running(&dw->work))
		was_pending = true;
	flush_work(&dw->work);
	return was_pending;
}

/* rcu_work: call_rcu runs the callback inline in the shim (no real grace period on the queue path),
 * so queuing rcu-work is equivalent to queuing the work immediately. */
bool queue_rcu_work(struct workqueue_struct *q, struct rcu_work *rw) {
	if (!rw) return false;
	rw->wq = q;
	return queue_work(q, &rw->work);
}
bool flush_rcu_work(struct rcu_work *rw) {
	if (!rw) return false;
	flush_work(&rw->work);
	return true;
}

void flush_workqueue(struct workqueue_struct *q) {
	if (!q) q = system_wq;
	if (!q) return;
	int guard = 1 << 20;
	for (;;) {
		int g2 = 4096;
		while (wq_run_one(q) && --g2 > 0) {}
		if (!q->current_w) {
			kt_lock(&g_lock);
			int empty = list_empty(&q->pending);
			kt_unlock(&g_lock);
			if (empty) break;
		}
		if (--guard <= 0) break;
		knx_thread_yield();
	}
}

bool cancel_work_sync(struct work_struct *w) {
	bool was;
	kt_lock(&g_lock);
	was = w->pending != 0;
	if (was) { list_del_init(&w->entry); w->pending = 0; }
	kt_unlock(&g_lock);
	int guard = 1 << 20;
	while (work_is_running(w) && --guard > 0)
		knx_thread_yield();
	return was;
}

static void wq_worker_body(void *arg) {
	struct workqueue_struct *q = (struct workqueue_struct *)arg;
	while (!knx_thread_should_stop()) {
		if (!wq_run_one(q))
			knx_thread_msleep(4);   /* idle: SLEEP (busy-yield would starve the run queue) */
	}
}

struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags, int max_active, ...) {
	(void)flags; (void)max_active;
	return wq_new(fmt);
}
struct workqueue_struct *create_singlethread_workqueue(const char *name) { return wq_new(name); }

void destroy_workqueue(struct workqueue_struct *q) {
	if (!q) return;
	flush_workqueue(q);
	if (q->worker) { knx_thread_stop(q->worker); q->worker = 0; }
	q->used = 0;
}

struct work_struct *current_work(void) { return 0; }

/* ======================= timers ======================= */

static struct list_head g_timers;
static int g_timers_inited = 0;

int mod_timer(struct timer_list *t, unsigned long expires) {
	int was;
	{ static int once; if (!once) { once = 1; printk("lkpi: FIRST timer armed (+%ld ms) — past GT unpark\n", (long)(expires - jiffies_now())); } }
	kt_lock(&g_tlock);
	was = t->lkpi_linked;
	t->expires = expires;
	if (!was) { list_add_tail(&t->entry, &g_timers); t->lkpi_linked = 1; }
	kt_unlock(&g_tlock);
	return was;
}
void add_timer(struct timer_list *t) { mod_timer(t, t->expires); }

int del_timer(struct timer_list *t) {
	int was;
	kt_lock(&g_tlock);
	was = t->lkpi_linked;
	if (was) { list_del_init(&t->entry); t->lkpi_linked = 0; }
	kt_unlock(&g_tlock);
	return was;
}
int del_timer_sync(struct timer_list *t)  { return del_timer(t); }
int timer_delete_sync(struct timer_list *t) { return del_timer(t); }

/* Fire every timer whose deadline has arrived (jiffies = ms). Called by the timer thread. */
static void timers_service(void) {
	unsigned long now = jiffies_now();
	for (;;) {
		struct timer_list *fire = 0;
		kt_lock(&g_tlock);
		struct list_head *p;
		list_for_each(p, &g_timers) {
			struct timer_list *t = list_entry(p, struct timer_list, entry);
			if ((long)(now - t->expires) >= 0) {   /* wrap-safe deadline reached */
				list_del_init(&t->entry);
				t->lkpi_linked = 0;
				fire = t;
				break;
			}
		}
		kt_unlock(&g_tlock);
		if (!fire)
			break;
		{ static int once; if (!once) { once = 1; printk("lkpi: FIRST timer fired\n"); } }
		if (fire->function)
			fire->function(fire);               /* run OUTSIDE the lock */
	}
}

static void timer_thread_body(void *arg) {
	(void)arg;
	while (!knx_thread_should_stop()) {
		timers_service();
		knx_thread_msleep(4);   /* ~4 ms timer resolution; sleep so we don't hog the CPU */
	}
}

/* Diagnostic/test hook: fire any due timers now (the timer thread calls timers_service in a loop). */
void lkpi_run_timers(void) { timers_service(); }

/* The cooperative wait pump (registered as the wait-pump hook, called from every __wait_event spin).
 * Fire due timers FIRST so delayed work armed with a real delay actually runs during a pre-scheduler
 * wait — jiffies is wall-clock, so a wait that spins `delay` ms crosses the deadline and the handler
 * fires ITERATIVELY (never recursively; a re-arm sets a future deadline this pass won't re-hit). Then
 * drain pending immediate work. Before this, the pump only drained the workqueue, so timer-backed
 * delayed work never advanced during a probe wait. */
static void lkpi_wq_pump(void) { timers_service(); lkpi_wq_drain(); }

/* ======================= delayed_work ======================= */

static void dwork_timer_fn(struct timer_list *t) {
	struct delayed_work *dw = from_timer(dw, t, timer);
	queue_work(dw->wq, &dw->work);
}

bool queue_delayed_work(struct workqueue_struct *q, struct delayed_work *dw, unsigned long delay) {
	if (!q) q = system_wq;
	dw->wq = q;
	/* Respect the delay even in the pre-scheduler INLINE regime (g_wq_async==0). The old shortcut ran
	 * delayed work inline with the delay DROPPED — which turns any self-re-arming periodic handler into
	 * unbounded recursion: i915's GT retire_work (intel_gt_requests.c:205) and engine heartbeat
	 * (intel_engine_heartbeat.c) re-queue THEMSELVES as their first act, so an inline call recurses on
	 * the same stack until it overflows -> #DF -> triple-fault reboot (observed at the FIRST GT unpark
	 * inside intel_gt_resume on the Dell, right after the workaround-init log lines). Arm the timer
	 * instead: jiffies is real wall-clock (knx_uptime_us), the deadline list exists pre-scheduler, and
	 * the cooperative wait pump services it (lkpi_wq_pump -> timers_service), so the handler fires
	 * ITERATIVELY at its real deadline (once per `delay`), exactly like mainline's timer->kworker. Only
	 * delay==0 keeps the immediate path. */
	if (delay == 0) {
		/* delay==0 must NOT run inline pre-scheduler either: this is the wakeref put_async path
		 * (intel_wakeref.c:79 mod_delayed_work(&wf->work, delay=0)) whose handler parks the engine/GT
		 * -> switch_to_kernel_context emits a request -> its retire calls intel_engine_pm_put_async ->
		 * mod_delayed_work(delay=0) again -> inline -> park -> ... mutual recursion. ENQUEUE instead;
		 * the cooperative wait pump drains it in process context, exactly like mainline's kworker.
		 * wq_enqueue is idempotent (guards on w->pending), so a re-put while queued is a no-op. Inline
		 * only survives when there is no system_wq yet (before lkpi_wq_init), i.e. no queue to hold it. */
		if (!q) { if (dw->work.func) dw->work.func(&dw->work); return true; }
		wq_enqueue(q, &dw->work);
		return true;
	}
	dw->timer.function = dwork_timer_fn;
	mod_timer(&dw->timer, jiffies_now() + delay);
	return true;
}
bool schedule_delayed_work(struct delayed_work *dw, unsigned long delay) {
	return queue_delayed_work(system_wq, dw, delay);
}
bool mod_delayed_work(struct workqueue_struct *q, struct delayed_work *dw, unsigned long delay) {
	del_timer(&dw->timer);
	return queue_delayed_work(q, dw, delay);
}
bool cancel_delayed_work(struct delayed_work *dw) {
	int had = del_timer(&dw->timer);
	return cancel_work_sync(&dw->work) || had;
}
bool cancel_delayed_work_sync(struct delayed_work *dw) { return cancel_delayed_work(dw); }

/* ======================= kthreads ======================= */

struct kthr { int (*fn)(void*); void *arg; struct task_struct ts; };
static void kthr_trampoline(void *p) {
	struct kthr *k = (struct kthr *)p;
	k->fn(k->arg);       /* loops until kthread_should_stop() -> knx_thread_should_stop() */
}

struct task_struct *kthread_run(int (*fn)(void*), void *arg, const char *name, ...) {
	struct kthr *k = (struct kthr *)kzalloc(sizeof(*k), 0);
	if (!k)
		return 0;
	k->fn = fn; k->arg = arg;
	k->ts.knx = knx_thread_spawn(kthr_trampoline, k, name);
	if (!k->ts.knx) { kfree(k); return 0; }
	return &k->ts;
}
int kthread_stop(struct task_struct *t) {
	if (!t || !t->knx)
		return -1;
	knx_thread_stop(t->knx);
	struct kthr *k = container_of(t, struct kthr, ts);
	kfree(k);
	return 0;
}
bool kthread_should_stop(void) { return knx_thread_should_stop() != 0; }

/* ======================= init / worker start ======================= */

/* lkpi_set_wq_pump (kpi_fence.c) is declared in <linux/workqueue.h> — registers the drain hook so
 * the cooperative wait pump also runs async work. */
static void *g_timer_thread;

/* Spawn every workqueue's worker + the timer thread, and flip to async. Deferred to post-scheduler
 * via knx_run_after_scheduler (kexts load before Scheduler::init). */
static void lkpi_wq_start_workers(void) {
#ifdef LKPI_WQ_INLINE
	/* Build-time escape for bisecting inline->async regressions: never flip to async; queue_work
	 * keeps running work inline (and the pump drain is a no-op since nothing is ever enqueued). */
	knx_log("lkpi: workqueues forced INLINE (LKPI_WQ_INLINE build)\n");
#else
	for (int i = 0; i < g_nwq; i++)
		if (!g_wqs[i].worker)
			g_wqs[i].worker = knx_thread_spawn(wq_worker_body, &g_wqs[i], "kworker");
	g_timer_thread = knx_thread_spawn(timer_thread_body, 0, "ktimers");
	g_wq_async = 1;      /* from here, queue_work enqueues instead of running inline */
	knx_log("lkpi: workqueues async (workers up) + timer thread\n");
#endif
}

/* Called once from the kext entry (nkext_init) BEFORE the driver probe, i.e. pre-scheduler: set up
 * the system queues + timer list, register the pump-drain hook, and defer worker spawn. */
void lkpi_wq_init(void) {
	if (!g_timers_inited) { INIT_LIST_HEAD(&g_timers); g_timers_inited = 1; }
	system_wq         = wq_new("system");
	system_highpri_wq = wq_new("system_highpri");
	system_long_wq    = wq_new("system_long");
	system_unbound_wq = wq_new("system_unbound");
	lkpi_set_wq_pump(lkpi_wq_pump);   /* timers + drain, so timer-backed delayed work advances in waits */
	knx_run_after_scheduler(lkpi_wq_start_workers);
	knx_log("lkpi: workqueues initialised (inline until scheduler up)\n");
}
