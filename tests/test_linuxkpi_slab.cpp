// test_linuxkpi_slab.cpp — host doctest for the LinuxKPI slab shim (kmalloc family).
// The shim's knx_malloc/knx_free are provided by tests/host_shims (forwarding to libc).
#include "doctest.h"
extern "C" {
#include "linux/slab.h"
}

TEST_CASE("kmalloc returns usable, kfree(NULL) is safe") {
    void *p = kmalloc(64, GFP_KERNEL);
    REQUIRE(p != nullptr);
    // payload must be 16-byte aligned
    CHECK(((unsigned long)p % 16) == 0);
    kfree(p);
    kfree(nullptr);  // no crash
}

TEST_CASE("kzalloc zeroes the whole block") {
    int *p = (int *)kzalloc(8 * sizeof(int), GFP_KERNEL);
    REQUIRE(p != nullptr);
    for (int i = 0; i < 8; i++) CHECK(p[i] == 0);
    kfree(p);
}

TEST_CASE("ksize reports the requested size") {
    void *p = kmalloc(123, GFP_KERNEL);
    CHECK(ksize(p) == 123);
    kfree(p);
    CHECK(ksize(nullptr) == 0);
}

TEST_CASE("krealloc preserves existing bytes (grow)") {
    unsigned char *p = (unsigned char *)kmalloc(4, GFP_KERNEL);
    for (int i = 0; i < 4; i++) p[i] = (unsigned char)(i + 1);
    p = (unsigned char *)krealloc(p, 16, GFP_KERNEL);
    REQUIRE(p != nullptr);
    for (int i = 0; i < 4; i++) CHECK(p[i] == (unsigned char)(i + 1));
    kfree(p);
}

TEST_CASE("krealloc truncates on shrink, NULL acts as malloc, 0 frees") {
    unsigned char *p = (unsigned char *)kmalloc(8, GFP_KERNEL);
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(i + 1);
    p = (unsigned char *)krealloc(p, 2, GFP_KERNEL);
    REQUIRE(p != nullptr);
    CHECK(ksize(p) == 2);
    CHECK(p[0] == 1);
    CHECK(p[1] == 2);
    CHECK(krealloc(p, 0, GFP_KERNEL) == nullptr);  // frees, returns NULL
    void *q = krealloc(nullptr, 32, GFP_KERNEL);   // acts as kmalloc
    REQUIRE(q != nullptr);
    kfree(q);
}

TEST_CASE("kcalloc zeroes and sizes n*size") {
    int *p = (int *)kcalloc(5, sizeof(int), GFP_KERNEL);
    REQUIRE(p != nullptr);
    CHECK(ksize(p) == 5 * sizeof(int));
    for (int i = 0; i < 5; i++) CHECK(p[i] == 0);
    kfree(p);
}

TEST_CASE("kstrdup copies the string incl NUL") {
    char *d = kstrdup("nanos", GFP_KERNEL);
    REQUIRE(d != nullptr);
    CHECK(ksize(d) == 6);
    CHECK(d[0] == 'n');
    CHECK(d[5] == '\0');
    kfree(d);
}
