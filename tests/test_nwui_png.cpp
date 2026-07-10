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

#include <cstdio>
#include <cstring>

/* Slurp the tiny 2x2 fixture so the corruption tests below mutate REAL encoder output. */
static unsigned char *slurp_fixture(long *len)
{
    FILE *f = fopen("tests/fixtures/icon_test.png", "rb");
    REQUIRE(f != nullptr);
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *) malloc((size_t) *len);
    REQUIRE(fread(buf, 1, (size_t) *len, f) == (size_t) *len);
    fclose(f);
    return buf;
}

TEST_CASE("png_decode rejects a corrupted read (chunk CRC) with a NAMED reason") {
    // The wallpaper-crash fix: bytes that don't match what the encoder wrote must FAIL the decode
    // (never feed garbage to the inflater), and the caller can log why (png_last_error).
    long len = 0;
    unsigned char *buf = slurp_fixture(&len);
    int w = 0, h = 0;
    buf[len / 2] ^= 0x40;                       // one flipped bit inside a chunk payload
    CHECK(png_decode(buf, (unsigned) len, &w, &h) == nullptr);
    CHECK(strcmp(png_last_error(), "chunk crc (corrupt read)") == 0);
    free(buf);
}

TEST_CASE("png_decode rejects a wrapped chunk length instead of a giant memcpy") {
    // Old bug: `off + 12 + clen > len` wrapped for a huge corrupted length field, passing the
    // bounds check and sending clen into memcpy (SIGSEGV) or looping the parser forever.
    long len = 0;
    unsigned char *buf = slurp_fixture(&len);
    int w = 0, h = 0;
    buf[8] = 0xFF; buf[9] = 0xFF; buf[10] = 0xFF; buf[11] = 0xF8;   // IHDR length -> 0xFFFFFFF8
    CHECK(png_decode(buf, (unsigned) len, &w, &h) == nullptr);
    CHECK(png_last_error()[0] != 0);            // named failure, not a crash
    free(buf);
}

TEST_CASE("png_decode handles truncation at every prefix: reject or decode CORRECTLY, never crash") {
    // A cut that only drops the trailing IEND still contains the complete image data — decoding
    // it is legitimate. The contract under truncation is: either a clean named reject, or the
    // exact same pixels as the full decode. (The crash-freedom itself is the primary oracle.)
    long len = 0;
    unsigned char *buf = slurp_fixture(&len);
    int w = 0, h = 0;
    uint32_t *ref = png_decode(buf, (unsigned) len, &w, &h);
    REQUIRE(ref != nullptr);
    REQUIRE(w == 2);
    REQUIRE(h == 2);
    for (long cut = 0; cut < len; cut++) {
        int cw = 0, ch = 0;
        uint32_t *px = png_decode(buf, (unsigned) cut, &cw, &ch);
        if (px) {
            CHECK(cw == w);
            CHECK(ch == h);
            CHECK(memcmp(px, ref, (size_t) w * h * 4) == 0);
            free(px);
        }
    }
    free(ref);
    free(buf);
}

TEST_CASE("png_decode rejects a bad signature and reports it") {
    unsigned char junk[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    int w = 0, h = 0;
    CHECK(png_decode(junk, sizeof junk, &w, &h) == nullptr);
    CHECK(strcmp(png_last_error(), "signature") == 0);
}
