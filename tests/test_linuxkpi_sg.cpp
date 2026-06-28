// test_linuxkpi_sg.cpp — host doctest for the LinuxKPI scatterlist (header-only).
#include "doctest.h"
extern "C" {
#include "linux/scatterlist.h"
}

TEST_CASE("sg_init_table zeroes and marks the last entry as end") {
    struct scatterlist sg[3];
    sg_init_table(sg, 3);
    CHECK(sg_is_last(&sg[2]) == 1);
    CHECK(sg_is_last(&sg[0]) == 0);
    CHECK(sg[0].length == 0);
}

TEST_CASE("sg_set_buf stores virt+len, sg_virt round-trips") {
    struct scatterlist sg[2];
    sg_init_table(sg, 2);
    static char buf0[64] __attribute__((aligned(16)));
    static char buf1[64] __attribute__((aligned(16)));
    sg_set_buf(&sg[0], buf0, 16);
    sg_set_buf(&sg[1], buf1, 32);
    CHECK(sg_virt(&sg[0]) == buf0);
    CHECK(sg_virt(&sg[1]) == buf1);
    CHECK(sg[0].length == 16);
    CHECK(sg[1].length == 32);
    // flags preserved across assign: entry 1 still marked end
    CHECK(sg_is_last(&sg[1]) == 1);
}

TEST_CASE("for_each_sg iterates exactly nents and sg_next stops at end") {
    struct scatterlist sg[4];
    sg_init_table(sg, 4);
    static char b[4][8] __attribute__((aligned(16)));
    for (int i = 0; i < 4; i++) sg_set_buf(&sg[i], b[i], 8);
    int count = 0;
    struct scatterlist *s; int i;
    for_each_sg(sg, s, 4, i) { (void)s; count++; }
    CHECK(count == 4);
    CHECK(sg_next(&sg[3]) == nullptr);
    CHECK(sg_next(&sg[0]) == &sg[1]);
}

TEST_CASE("sg_phys/dma accessors reflect the buffer address") {
    struct scatterlist sg[1];
    sg_init_one(sg, (void*)0x100000, 4096);
    CHECK(sg_phys(&sg[0]) == (dma_addr_t)0x100000);
    sg_dma_address(&sg[0]) = 0x200000;
    sg_dma_len(&sg[0]) = 4096;
    CHECK(sg_dma_address(&sg[0]) == (dma_addr_t)0x200000);
    CHECK(sg_dma_len(&sg[0]) == 4096u);
}
