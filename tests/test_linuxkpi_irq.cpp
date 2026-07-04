// test_linuxkpi_irq.cpp — host doctest for the LinuxKPI irq descriptor table (kpi_irq.c).
// Exercises request_irq / request_threaded_irq / free_irq / lkpi_irq_dispatch without a real MSI
// (the shim irq numbers start at 32; slots 0..2 => irq 32..34 here).
#include "doctest.h"
extern "C" {
#include "linux/interrupt.h"
}

static int   g_calls = 0;
static void *g_last_dev = 0;
static irqreturn_t test_handler(int irq, void *dev) { (void)irq; g_calls++; g_last_dev = dev; return IRQ_HANDLED; }

static int   g_thread_calls = 0;
static irqreturn_t wake_handler(int irq, void *dev) { (void)irq; (void)dev; return IRQ_WAKE_THREAD; }
static irqreturn_t thread_fn(int irq, void *dev)    { (void)irq; (void)dev; g_thread_calls++; return IRQ_HANDLED; }

TEST_CASE("request_irq installs a handler that lkpi_irq_dispatch invokes with the dev cookie") {
	g_calls = 0; g_last_dev = 0;
	int dummy;
	CHECK(request_irq(32, test_handler, 0, "t", &dummy) == 0);
	lkpi_irq_dispatch(32);
	CHECK(g_calls == 1);
	CHECK(g_last_dev == &dummy);
	free_irq(32, &dummy);
}

TEST_CASE("free_irq stops further dispatch; double-free is a harmless no-op") {
	g_calls = 0;
	int dummy;
	request_irq(33, test_handler, 0, "t", &dummy);
	lkpi_irq_dispatch(33);
	CHECK(g_calls == 1);
	free_irq(33, &dummy);
	lkpi_irq_dispatch(33);            // handler cleared -> no call
	CHECK(g_calls == 1);
	free_irq(33, &dummy);             // double free -> no crash, still cleared
	lkpi_irq_dispatch(33);
	CHECK(g_calls == 1);
}

TEST_CASE("request_irq rejects out-of-range irq numbers") {
	int dummy;
	CHECK(request_irq(0, test_handler, 0, "t", &dummy) != 0);      // below LKPI_IRQ_BASE
	CHECK(request_irq(9999, test_handler, 0, "t", &dummy) != 0);   // above the table
}

TEST_CASE("request_threaded_irq runs thread_fn inline on IRQ_WAKE_THREAD") {
	g_thread_calls = 0;
	int dummy;
	CHECK(request_threaded_irq(34, wake_handler, thread_fn, 0, "t", &dummy) == 0);
	lkpi_irq_dispatch(34);
	CHECK(g_thread_calls == 1);
	free_irq(34, &dummy);
}

TEST_CASE("dispatch of an unbound irq is a harmless no-op") {
	lkpi_irq_dispatch(40);           // never requested
	CHECK(true);
}
