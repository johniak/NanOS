// test_linuxkpi_time.cpp — host doctest for LinuxKPI jiffies/time conversions.
// Provides a controllable knx_uptime_us so lkpi_jiffies is deterministic.
#include "doctest.h"
extern "C" {
#include "linux/jiffies.h"
}

static unsigned long long g_uptime_us = 0;
extern "C" unsigned long long knx_uptime_us(void) { return g_uptime_us; }

TEST_CASE("HZ=1000: ms<->jiffies are 1:1") {
    CHECK(msecs_to_jiffies(0) == 0);
    CHECK(msecs_to_jiffies(250) == 250);
    CHECK(jiffies_to_msecs(1000) == 1000u);
}

TEST_CASE("usecs_to_jiffies rounds up to whole ms") {
    CHECK(usecs_to_jiffies(0) == 0);
    CHECK(usecs_to_jiffies(1) == 1);      // <1ms rounds up to 1 jiffy
    CHECK(usecs_to_jiffies(1000) == 1);   // exactly 1ms
    CHECK(usecs_to_jiffies(1001) == 2);   // just over 1ms
    CHECK(usecs_to_jiffies(2000) == 2);
}

TEST_CASE("jiffies<->nsecs") {
    CHECK(jiffies_to_nsecs(1) == 1000000ull);
    CHECK(nsecs_to_jiffies(5000000ull) == 5);
}

TEST_CASE("lkpi_jiffies derives ms from uptime us") {
    g_uptime_us = 0;            CHECK(lkpi_jiffies() == 0);
    g_uptime_us = 1500;         CHECK(lkpi_jiffies() == 1);   // 1.5ms -> 1 jiffy
    g_uptime_us = 5000000ull;   CHECK(lkpi_jiffies() == 5000);
    // `jiffies` macro reads live
    g_uptime_us = 7000;         CHECK(jiffies == 7);
}

TEST_CASE("time_after/before correct in the normal range") {
    unsigned long a = 100, b = 200;
    CHECK(time_after(b, a));
    CHECK(!time_after(a, b));
    CHECK(time_before(a, b));
    CHECK(time_after_eq(b, b));
    CHECK(time_before_eq(a, a));
}

TEST_CASE("time_after stays correct across an unsigned-long wrap") {
    unsigned long max = (unsigned long)-1;   // ULONG_MAX
    unsigned long before = max - 2;
    unsigned long after = 1;                  // wrapped past max
    CHECK(time_after(after, before));         // 1 is after MAX-2 across the wrap
    CHECK(!time_after(before, after));
}
