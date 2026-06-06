// doctest test runner entry point. Test cases live in the test_*.cpp files
// alongside this one; they just #include "doctest.h".
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

TEST_CASE("harness sanity") {
	CHECK(1 + 1 == 2);
}
