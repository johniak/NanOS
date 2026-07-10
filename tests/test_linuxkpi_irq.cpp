// test_linuxkpi_irq.cpp — host doctest for the LinuxKPI irq descriptor table (kpi_irq.c).
// Exercises request_irq / request_threaded_irq / free_irq / lkpi_irq_dispatch / lkpi_irq_poll
// without a real MSI (the shim irq numbers start at 32; slots 0..2 => irq 32..34 here).
//
// CONTRACT (latch-only, the cross-core fix): the hard-IRQ frame (lkpi_irq_dispatch) runs NO
// driver code — running gen8_irq_handler in interrupt context on the fixed MSI core raced the
// nwm thread's i915 section on another core under the shim's no-op locks (the Dell GL freeze
// class). Handlers execute exclusively in the pump HARVEST (lkpi_irq_poll), in thread context
// under the one-executor gate. These tests pin both halves of that contract.
#include "doctest.h"
extern "C" {
#include "linux/interrupt.h"
}
/* Plain (C++) linkage on the host: kpi_irq.c is compiled with g++ there, and these two are
 * declared nowhere in the wrapped headers (the lkpi_wait_pump plain-linkage convention). */
void lkpi_irq_poll(void);
void lkpi_irq_test_bind(int irq);   // NANOS_HOST_TEST only: mark a slot bound (no real MSI on host)

static int   g_calls = 0;
static void *g_last_dev = 0;
static irqreturn_t test_handler(int irq, void *dev) { (void)irq; g_calls++; g_last_dev = dev; return IRQ_HANDLED; }

static int   g_thread_calls = 0;
static irqreturn_t wake_handler(int irq, void *dev) { (void)irq; (void)dev; return IRQ_WAKE_THREAD; }
static irqreturn_t thread_fn(int irq, void *dev)    { (void)irq; (void)dev; g_thread_calls++; return IRQ_HANDLED; }

TEST_CASE("lkpi_irq_dispatch is LATCH-ONLY: no handler runs in the hard-IRQ frame") {
	g_calls = 0; g_last_dev = 0;
	int dummy;
	CHECK(request_irq(32, test_handler, 0, "t", &dummy) == 0);
	lkpi_irq_dispatch(32);
	CHECK(g_calls == 0);              // the frame must NOT execute driver code (cross-core fix)
	free_irq(32, &dummy);
}

TEST_CASE("the pump harvest (lkpi_irq_poll) runs a bound handler with its dev cookie") {
	g_calls = 0; g_last_dev = 0;
	int dummy;
	CHECK(request_irq(32, test_handler, 0, "t", &dummy) == 0);
	lkpi_irq_test_bind(32);
	lkpi_irq_poll();
	CHECK(g_calls == 1);
	CHECK(g_last_dev == &dummy);
	free_irq(32, &dummy);
}

TEST_CASE("free_irq stops the harvest; double-free is a harmless no-op") {
	g_calls = 0;
	int dummy;
	request_irq(33, test_handler, 0, "t", &dummy);
	lkpi_irq_test_bind(33);
	lkpi_irq_poll();
	CHECK(g_calls == 1);
	free_irq(33, &dummy);
	lkpi_irq_poll();                  // handler cleared -> no call
	CHECK(g_calls == 1);
	free_irq(33, &dummy);             // double free -> no crash, still cleared
	lkpi_irq_poll();
	CHECK(g_calls == 1);
}

TEST_CASE("request_irq rejects out-of-range irq numbers") {
	int dummy;
	CHECK(request_irq(0, test_handler, 0, "t", &dummy) != 0);      // below LKPI_IRQ_BASE
	CHECK(request_irq(9999, test_handler, 0, "t", &dummy) != 0);   // above the table
}

TEST_CASE("the harvest runs thread_fn inline on IRQ_WAKE_THREAD") {
	g_thread_calls = 0;
	int dummy;
	CHECK(request_threaded_irq(34, wake_handler, thread_fn, 0, "t", &dummy) == 0);
	lkpi_irq_test_bind(34);
	lkpi_irq_poll();
	CHECK(g_thread_calls == 1);
	free_irq(34, &dummy);
}

TEST_CASE("dispatch of an unbound irq is a harmless no-op") {
	lkpi_irq_dispatch(40);           // never requested
	CHECK(true);
}
