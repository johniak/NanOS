// Host doctests for the LinuxKPI shrinker registry (linuxkpi/kpi_shrinker.c). The registry is real
// (alloc/link/unlink/free); reclaim is intentionally never invoked (a boot notice records that).
#include "doctest.h"
extern "C" {
#include "linux/shrinker.h"
}

static unsigned long count_cb(struct shrinker*, struct shrink_control*) { return 0; }
static unsigned long scan_cb(struct shrinker*, struct shrink_control*)  { return SHRINK_STOP; }

TEST_CASE("shrinker_alloc zeroes the struct and sets defaults") {
	struct shrinker* s = shrinker_alloc(0, "test");
	REQUIRE(s != 0);
	CHECK(s->seeks == DEFAULT_SEEKS);
	CHECK(s->flags == 0u);
	CHECK(s->batch == 0);
	CHECK(s->count_objects == 0);
	CHECK(s->scan_objects == 0);
	CHECK(s->private_data == 0);
	shrinker_free(s);
}

TEST_CASE("shrinker_alloc records flags; register links, free unlinks") {
	int base = lkpi_shrinker_registered_count();
	struct shrinker* s = shrinker_alloc(SHRINKER_NUMA_AWARE, "gpu-%d", 1);
	REQUIRE(s != 0);
	CHECK((s->flags & SHRINKER_NUMA_AWARE) != 0u);
	s->count_objects = count_cb;
	s->scan_objects  = scan_cb;
	s->private_data  = (void*)s;
	shrinker_register(s);
	CHECK(lkpi_shrinker_registered_count() == base + 1);
	shrinker_free(s);
	CHECK(lkpi_shrinker_registered_count() == base);
}

TEST_CASE("two shrinkers register and free independently") {
	int base = lkpi_shrinker_registered_count();
	struct shrinker* a = shrinker_alloc(0, "a");
	struct shrinker* b = shrinker_alloc(0, "b");
	shrinker_register(a);
	shrinker_register(b);
	CHECK(lkpi_shrinker_registered_count() == base + 2);
	shrinker_free(a);
	CHECK(lkpi_shrinker_registered_count() == base + 1);
	shrinker_free(b);
	CHECK(lkpi_shrinker_registered_count() == base);
}

TEST_CASE("shrinker_free on an unregistered shrinker is safe (no registry change)") {
	int base = lkpi_shrinker_registered_count();
	struct shrinker* s = shrinker_alloc(0, "orphan");
	shrinker_free(s);   // never registered; INIT_LIST_HEAD'd self-link makes list_del a no-op
	CHECK(lkpi_shrinker_registered_count() == base);
}
