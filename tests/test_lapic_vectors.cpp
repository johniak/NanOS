#include "doctest.h"
#include "lapic_x86_64.h"
using namespace kernel;

TEST_CASE("msiVecAlloc hands out distinct vectors in range then signals exhaustion") {
	MsiVecPool p;
	msiVecPoolInit(&p);
	int first = msiVecAlloc(&p);
	CHECK(first == MSI_VEC_BASE);
	int second = msiVecAlloc(&p);
	CHECK(second == MSI_VEC_BASE + 1);
	CHECK(second != first);
	// Drain the rest of the pool.
	int count = 2;
	while (msiVecAlloc(&p) >= 0) count++;
	CHECK(count == (MSI_VEC_END - MSI_VEC_BASE));   // exactly the pool size handed out
	CHECK(msiVecAlloc(&p) == -1);                   // exhausted
}
