// test_linuxkpi_idr.cpp — host doctest for the LinuxKPI idr/ida shim.
#include "doctest.h"
extern "C" {
#include "linux/idr.h"
}

TEST_CASE("idr_alloc hands out increasing ids, find returns stored ptr") {
    struct idr idr; idr_init(&idr);
    int a = 10, b = 20, c = 30;
    int i0 = idr_alloc(&idr, &a, 0, 0, 0);
    int i1 = idr_alloc(&idr, &b, 0, 0, 0);
    int i2 = idr_alloc(&idr, &c, 0, 0, 0);
    CHECK(i0 == 0); CHECK(i1 == 1); CHECK(i2 == 2);
    CHECK(idr_find(&idr, 0) == &a);
    CHECK(idr_find(&idr, 2) == &c);
    CHECK(idr_find(&idr, 99) == nullptr);
    idr_destroy(&idr);
}

TEST_CASE("idr_remove frees the slot and the id is recycled lowest-first") {
    struct idr idr; idr_init(&idr);
    int a=1,b=2,c=3;
    idr_alloc(&idr,&a,0,0,0);
    int i1 = idr_alloc(&idr,&b,0,0,0);
    idr_alloc(&idr,&c,0,0,0);
    CHECK(idr_remove(&idr, i1) == &b);
    CHECK(idr_find(&idr, i1) == nullptr);
    int reuse = idr_alloc(&idr, &b, 0, 0, 0);  // lowest free is the recycled id1
    CHECK(reuse == i1);
    idr_destroy(&idr);
}

TEST_CASE("idr_alloc honours start and end range") {
    struct idr idr; idr_init(&idr);
    int x=1;
    int id = idr_alloc(&idr, &x, 5, 8, 0);
    CHECK(id == 5);
    // fill 6,7 then range [5,8) is exhausted
    idr_alloc(&idr,&x,5,8,0);
    idr_alloc(&idr,&x,5,8,0);
    int full = idr_alloc(&idr,&x,5,8,0);
    CHECK(full < 0);  // -ENOSPC
    idr_destroy(&idr);
}

TEST_CASE("idr_init_base shifts the lowest id") {
    struct idr idr; idr_init_base(&idr, 1);
    int x=1;
    int id = idr_alloc(&idr, &x, 0, 0, 0);
    CHECK(id == 1);  // base=1 so id 0 is skipped
    idr_destroy(&idr);
}

TEST_CASE("idr_replace swaps the pointer, keeps the id") {
    struct idr idr; idr_init(&idr);
    int a=1,b=2;
    int id = idr_alloc(&idr,&a,0,0,0);
    idr_replace(&idr,&b,id);
    CHECK(idr_find(&idr,id) == &b);
    idr_destroy(&idr);
}

TEST_CASE("ida allocates from 0 incl, free recycles") {
    struct ida ida; ida_init(&ida);
    int a = ida_alloc(&ida, 0);
    int b = ida_alloc(&ida, 0);
    CHECK(a == 0); CHECK(b == 1);
    ida_free(&ida, a);
    int reuse = ida_alloc(&ida, 0);
    CHECK(reuse == 0);
    ida_destroy(&ida);
}

TEST_CASE("idr_is_empty reflects contents") {
    struct idr idr; idr_init(&idr);
    CHECK(idr_is_empty(&idr) == 1);
    int x=1; int id = idr_alloc(&idr,&x,0,0,0);
    CHECK(idr_is_empty(&idr) == 0);
    idr_remove(&idr, id);
    CHECK(idr_is_empty(&idr) == 1);
    idr_destroy(&idr);
}
