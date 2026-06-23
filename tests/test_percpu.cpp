#include "doctest.h"
#include "percpu_x86_64.h"
using namespace arch;

TEST_CASE("cpuIndexForLapic maps a LAPIC id to its dense index") {
	uint8_t ids[4] = { 0, 1, 3, 7 };   // enabled CPUs, dense indices 0..3
	CHECK(cpuIndexForLapic(ids, 4, 0) == 0);
	CHECK(cpuIndexForLapic(ids, 4, 1) == 1);
	CHECK(cpuIndexForLapic(ids, 4, 3) == 2);
	CHECK(cpuIndexForLapic(ids, 4, 7) == 3);
}

TEST_CASE("cpuIndexForLapic returns -1 for an unknown id") {
	uint8_t ids[2] = { 0, 4 };
	CHECK(cpuIndexForLapic(ids, 2, 9) == -1);
	CHECK(cpuIndexForLapic(ids, 0, 0) == -1);   // empty table
}

TEST_CASE("PerCpu keeps the syscall-stub field offsets fixed") {
	// syscall_entry64.S hard-codes %gs:0 and %gs:8 — guard those offsets.
	CHECK(__builtin_offsetof(PerCpu, kernelStackTop) == 0);
	CHECK(__builtin_offsetof(PerCpu, userRspScratch) == 8);
}
