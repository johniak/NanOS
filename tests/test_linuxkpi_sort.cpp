// test_linuxkpi_sort.cpp — host doctest for the LinuxKPI sort()/bsearch().
#include "doctest.h"
extern "C" {
#include "linux/sort.h"
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

TEST_CASE("sort orders an int array ascending") {
    int a[] = {5, 1, 4, 2, 8, 0, 3, 7, 6, 9};
    sort(a, 10, sizeof(int), cmp_int, nullptr);
    for (int i = 0; i < 10; i++) CHECK(a[i] == i);
}

TEST_CASE("sort handles duplicates and already-sorted/reversed") {
    int dup[] = {3, 1, 3, 2, 1};
    sort(dup, 5, sizeof(int), cmp_int, nullptr);
    int exp[] = {1, 1, 2, 3, 3};
    for (int i = 0; i < 5; i++) CHECK(dup[i] == exp[i]);

    int rev[] = {9,8,7,6,5};
    sort(rev, 5, sizeof(int), cmp_int, nullptr);
    for (int i = 0; i < 5; i++) CHECK(rev[i] == 5 + i);
}

TEST_CASE("sort no-ops on 0 or 1 element") {
    int one[] = {42};
    sort(one, 1, sizeof(int), cmp_int, nullptr);
    CHECK(one[0] == 42);
}

static int swap_calls = 0;
static void cnt_swap(void *a, void *b, int size) {
    swap_calls++;
    char *x = (char*)a, *y = (char*)b;
    for (int i = 0; i < size; i++) { char t = x[i]; x[i] = y[i]; y[i] = t; }
}

TEST_CASE("sort uses the provided swap callback") {
    swap_calls = 0;
    int a[] = {3, 2, 1};
    sort(a, 3, sizeof(int), cmp_int, cnt_swap);
    CHECK(a[0] == 1); CHECK(a[1] == 2); CHECK(a[2] == 3);
    CHECK(swap_calls > 0);
}

TEST_CASE("bsearch finds present, misses absent (on sorted array)") {
    int a[] = {1, 3, 5, 7, 9, 11};
    int key = 7;
    int *hit = (int *)lkpi_bsearch(&key, a, 6, sizeof(int), cmp_int);
    REQUIRE(hit != nullptr);
    CHECK(*hit == 7);
    int miss = 8;
    CHECK(lkpi_bsearch(&miss, a, 6, sizeof(int), cmp_int) == nullptr);
}
