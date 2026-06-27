#include "doctest.h"
#include "nwui_fs.h"
#include <cstring>

TEST_CASE("nwui_dir enumerates a real directory, skipping . and ..") {
    void *d = nwui_dir_open("tests/fixtures");
    REQUIRE(d != nullptr);
    char name[256];
    int is_dir = 0, count = 0, saw_icon = 0;
    while (nwui_dir_next(d, name, sizeof name, &is_dir) == 1) {
        CHECK(strcmp(name, ".") != 0);
        CHECK(strcmp(name, "..") != 0);
        if (strcmp(name, "icon_test.png") == 0) saw_icon = 1;
        count++;
    }
    nwui_dir_close(d);
    CHECK(count > 0);
    CHECK(saw_icon == 1);   // the committed PNG fixture is there
}

TEST_CASE("nwui_dir_open returns null for a missing directory") {
    void *d = nwui_dir_open("tests/fixtures/nope-not-here");
    CHECK(d == nullptr);
    char name[16]; int is_dir = 0;
    CHECK(nwui_dir_next(d, name, sizeof name, &is_dir) == 0);  // null handle -> end
}
