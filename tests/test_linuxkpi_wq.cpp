// test_linuxkpi_wq.cpp — host doctest for the LinuxKPI async workqueues, timers and kthreads
// (kpi_kthread.c). The host stubs (host_shims.cpp) make knx_run_after_scheduler fire immediately, so
// lkpi_wq_init() leaves the queues in ASYNC mode; knx_thread_spawn returns a dummy (no real worker),
// so the test itself drives lkpi_wq_drain()/lkpi_run_timers() to model the worker/timer thread.
#include "doctest.h"
extern "C" {
#include "linux/workqueue.h"
#include "linux/timer.h"
#include "linux/kthread.h"
#include "linux/sched.h"
}

static void ensure_wq() {
	static bool once = false;
	if (!once) { lkpi_wq_init(); once = true; }   // sets up system_wq + flips async (stub)
}

static int g_ran = 0;
static void wfn(struct work_struct *) { g_ran++; }

TEST_CASE("queue_work enqueues in async mode; lkpi_wq_drain runs it once") {
	ensure_wq();
	struct work_struct w; INIT_WORK(&w, wfn); g_ran = 0;
	CHECK(queue_work(system_wq, &w) == true);
	CHECK(work_pending(&w));       // queued, not yet run
	CHECK(g_ran == 0);
	lkpi_wq_drain();
	CHECK(!work_pending(&w));
	CHECK(g_ran == 1);
	lkpi_wq_drain();               // idempotent: nothing pending
	CHECK(g_ran == 1);
}

TEST_CASE("schedule_work routes to system_wq; flush_work runs the pending work") {
	ensure_wq();
	struct work_struct w; INIT_WORK(&w, wfn); g_ran = 0;
	schedule_work(&w);
	CHECK(work_pending(&w));
	flush_work(&w);
	CHECK(g_ran == 1);
	CHECK(!work_pending(&w));
}

TEST_CASE("cancel_work_sync removes a pending work and reports whether it was pending") {
	ensure_wq();
	struct work_struct w; INIT_WORK(&w, wfn); g_ran = 0;
	queue_work(system_wq, &w);
	CHECK(cancel_work_sync(&w) == true);   // was pending
	CHECK(!work_pending(&w));
	lkpi_wq_drain();
	CHECK(g_ran == 0);                      // canceled -> never ran
	CHECK(cancel_work_sync(&w) == false);   // no longer pending
}

TEST_CASE("re-queuing the same work while pending is idempotent (single run)") {
	ensure_wq();
	struct work_struct w; INIT_WORK(&w, wfn); g_ran = 0;
	queue_work(system_wq, &w);
	queue_work(system_wq, &w);   // already pending -> not double-added
	lkpi_wq_drain();
	CHECK(g_ran == 1);
}

static int g_timer_fired = 0;
static void tfn(struct timer_list *) { g_timer_fired++; }

TEST_CASE("mod_timer arms; a due timer fires via lkpi_run_timers; del_timer disarms") {
	ensure_wq();
	struct timer_list t; timer_setup(&t, tfn, 0); g_timer_fired = 0;
	mod_timer(&t, 0);              // expires 0 -> due immediately
	CHECK(t.lkpi_linked == 1);
	lkpi_run_timers();
	CHECK(g_timer_fired == 1);
	CHECK(t.lkpi_linked == 0);     // unlinked after firing
	mod_timer(&t, 0);
	CHECK(del_timer(&t) == 1);     // was armed
	CHECK(t.lkpi_linked == 0);
	g_timer_fired = 0;
	lkpi_run_timers();
	CHECK(g_timer_fired == 0);     // disarmed -> does not fire
}

TEST_CASE("queue_delayed_work(delay=0) enqueues the work immediately") {
	ensure_wq();
	struct delayed_work dw; INIT_DELAYED_WORK(&dw, wfn); g_ran = 0;
	queue_delayed_work(system_wq, &dw, 0);
	CHECK(work_pending(&dw.work));
	lkpi_wq_drain();
	CHECK(g_ran == 1);
}

TEST_CASE("delayed_work's timer, when due, queues the work (timer -> work chain)") {
	ensure_wq();
	struct delayed_work dw; INIT_DELAYED_WORK(&dw, wfn); g_ran = 0;
	queue_delayed_work(system_wq, &dw, 50);   // arms dw.timer at now+50
	CHECK(!work_pending(&dw.work));            // not queued yet
	del_timer(&dw.timer); mod_timer(&dw.timer, 0);   // force it due now
	lkpi_run_timers();                          // fires -> queue_work(&dw.work)
	CHECK(work_pending(&dw.work));
	lkpi_wq_drain();
	CHECK(g_ran == 1);
}

static int ktfn(void *) { return 0; }

TEST_CASE("kthread_run returns a task; should_stop is false; stop frees it") {
	struct task_struct *t = kthread_run(ktfn, 0, "t");
	CHECK(t != nullptr);
	CHECK(kthread_should_stop() == false);
	CHECK(kthread_stop(t) == 0);
}
