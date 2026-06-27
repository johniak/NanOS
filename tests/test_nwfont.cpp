#include "doctest.h"
#include "nwfont.h"

TEST_CASE("nwfont loads a TTF and reports proportional metrics") {
    int rc = nwfont_set(NWFONT_UI, "assets/fonts/UISans-Regular.ttf", 18);
    REQUIRE(rc == 0);
    CHECK(nwfont_loaded(NWFONT_UI) == 1);
    CHECK(nwfont_line_h(NWFONT_UI) > 0);
    CHECK(nwfont_ascent(NWFONT_UI) > 0);

    CHECK(nwfont_text_w(NWFONT_UI, "") == 0);
    int w1 = nwfont_text_w(NWFONT_UI, "W");
    int w2 = nwfont_text_w(NWFONT_UI, "WW");
    CHECK(w1 > 0);
    CHECK(w2 > w1);
    // proportional: 'i' is narrower than 'W'
    CHECK(nwfont_text_w(NWFONT_UI, "i") < nwfont_text_w(NWFONT_UI, "W"));

    const struct nwfont_glyph *A = nwfont_get(NWFONT_UI, 'A');
    REQUIRE(A != nullptr);
    CHECK(A->w > 0);
    CHECK(A->h > 0);
    CHECK(A->advance > 0);
    CHECK(A->cov != nullptr);

    const struct nwfont_glyph *sp = nwfont_get(NWFONT_UI, ' ');
    REQUIRE(sp != nullptr);
    CHECK(sp->advance > 0);    // space advances...
    CHECK(sp->w == 0);         // ...but has no bitmap
}

TEST_CASE("nwfont_set fails for a missing file") {
    CHECK(nwfont_set(NWFONT_MONO, "assets/fonts/does-not-exist.ttf", 16) < 0);
    CHECK(nwfont_loaded(NWFONT_MONO) == 0);
}
