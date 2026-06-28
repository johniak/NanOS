// test_linuxkpi_print.cpp — host doctest for the LinuxKPI vsnprintf subset.
// Tests the collision-free entry points (vscnprintf/scnprintf); snprintf/sprintf/vsnprintf
// are the kext-only libc-named wrappers (guarded out under NANOS_HOST_TEST).
#include "doctest.h"
#include <cstring>
extern "C" {
#include "linux/printk.h"
}

static const char *F(char *b, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vscnprintf(b, n, fmt, ap);
    va_end(ap);
    return b;
}

TEST_CASE("integers: signed, unsigned, hex") {
    char b[64];
    CHECK(std::strcmp(F(b, sizeof b, "%d", 42), "42") == 0);
    CHECK(std::strcmp(F(b, sizeof b, "%d", -7), "-7") == 0);
    CHECK(std::strcmp(F(b, sizeof b, "%u", 4000000000u), "4000000000") == 0);
    CHECK(std::strcmp(F(b, sizeof b, "%x", 0xDEADu), "dead") == 0);
    CHECK(std::strcmp(F(b, sizeof b, "%X", 0xBEEFu), "BEEF") == 0);
    CHECK(std::strcmp(F(b, sizeof b, "%o", 8u), "10") == 0);
}

TEST_CASE("width, zero-pad, left-justify, precision") {
    char b[64];
    CHECK(std::strcmp(F(b, sizeof b, "%5d", 42), "   42") == 0);
    CHECK(std::strcmp(F(b, sizeof b, "%-5d|", 42), "42   |") == 0);
    CHECK(std::strcmp(F(b, sizeof b, "%05d", 42), "00042") == 0);
    CHECK(std::strcmp(F(b, sizeof b, "%08x", 0x1234u), "00001234") == 0);
    CHECK(std::strcmp(F(b, sizeof b, "%.3d", 5), "005") == 0);
    CHECK(std::strcmp(F(b, sizeof b, "%+d", 5), "+5") == 0);
}

TEST_CASE("strings and chars, precision truncation") {
    char b[64];
    CHECK(std::strcmp(F(b, sizeof b, "%s", "nanos"), "nanos") == 0);
    CHECK(std::strcmp(F(b, sizeof b, "%.3s", "nanos"), "nan") == 0);
    CHECK(std::strcmp(F(b, sizeof b, "%8s", "ab"), "      ab") == 0);
    CHECK(std::strcmp(F(b, sizeof b, "%c%c", 'O', 'K'), "OK") == 0);
    CHECK(std::strcmp(F(b, sizeof b, "%s", (char*)nullptr), "(null)") == 0);
}

TEST_CASE("length modifiers: long, long long, size_t") {
    char b[64];
    CHECK(std::strcmp(F(b, sizeof b, "%ld", 1234567890L), "1234567890") == 0);
    CHECK(std::strcmp(F(b, sizeof b, "%lld", 9000000000LL), "9000000000") == 0);
    CHECK(std::strcmp(F(b, sizeof b, "%zu", (size_t)4096), "4096") == 0);
    CHECK(std::strcmp(F(b, sizeof b, "%llx", 0xCAFEBABEULL), "cafebabe") == 0);
}

TEST_CASE("pointer variants: %p %px %pK raw hex, %pa deref") {
    char b[64];
    void *ptr = (void*)0x1000;
    CHECK(std::strcmp(F(b, sizeof b, "%p", ptr), "0x1000") == 0);
    CHECK(std::strcmp(F(b, sizeof b, "%px", ptr), "0x1000") == 0);
    CHECK(std::strcmp(F(b, sizeof b, "%pK", ptr), "0x1000") == 0);
    phys_addr_t pa = 0xFEE00000ULL;
    CHECK(std::strcmp(F(b, sizeof b, "%pa", &pa), "0xfee00000") == 0);
}

TEST_CASE("literal percent and mixed") {
    char b[64];
    CHECK(std::strcmp(F(b, sizeof b, "100%%"), "100%") == 0);
    CHECK(std::strcmp(F(b, sizeof b, "%s=%d (0x%x)", "id", 16, 16u), "id=16 (0x10)") == 0);
}

TEST_CASE("vscnprintf clamps and NUL-terminates; would-be length via vsnprintf-ish") {
    char b[5];
    int n = scnprintf(b, sizeof b, "%s", "abcdefgh"); // capacity 5 -> writes 4 + NUL
    CHECK(n == 4);
    CHECK(std::strcmp(b, "abcd") == 0);
}
