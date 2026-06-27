#include "doctest.h"
#include "nwui_png.h"
#include <stdlib.h>   /* free (libc; memory_manager.h is not linked in host tests) */

TEST_CASE("nwui_image_load_png decodes a known 2x2 RGB PNG") {
    int w = 0, h = 0;
    uint32_t *px = nwui_image_load_png("tests/fixtures/icon_test.png", &w, &h);
    REQUIRE(px != nullptr);
    CHECK(w == 2);
    CHECK(h == 2);
    CHECK((px[0] & 0x00ffffff) == 0x00ff0000); // red
    CHECK((px[1] & 0x00ffffff) == 0x0000ff00); // green
    CHECK((px[2] & 0x00ffffff) == 0x000000ff); // blue
    CHECK((px[3] & 0x00ffffff) == 0x00ffffff); // white
    free(px);
}

TEST_CASE("nwui_image_load_png returns null for a missing file") {
    int w = 0, h = 0;
    CHECK(nwui_image_load_png("tests/fixtures/does_not_exist.png", &w, &h) == nullptr);
}
