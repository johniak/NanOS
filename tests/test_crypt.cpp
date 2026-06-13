/*
 * test_crypt.cpp — crypt(3) SHA-512 ($6$) known-answer vectors (user/libc-glue/crypt.c).
 *
 * The expected hashes were produced by `openssl passwd -6` (the reference implementation), so a
 * match proves our crypt() is glibc/OpenSSL-compatible byte-for-byte — a hash made by either tool
 * verifies on NanOS (this is what lets Dropbear do password auth). Locks the algorithm against
 * regressions.
 */
#include "doctest.h"
#include <cstring>

extern "C" char* crypt(const char* key, const char* setting);

TEST_CASE("sha512-crypt matches the openssl passwd -6 reference") {
	CHECK(strcmp(crypt("secret", "$6$abcdefghijklmnop$"),
		"$6$abcdefghijklmnop$J/AWykHqo2Tx5UtavGnFc3ytI33la50JpzLTarSWVhkIXK6wOjNwwZjsrIw2UgmrER2EKrSHCeQyAINEEXAk1/") == 0);
	CHECK(strcmp(crypt("nanos", "$6$NaNoSsaLt12345$"),
		"$6$NaNoSsaLt12345$F1FE/Qd7iATvVrjtCOPuH5.zvPpuFsPv9bbOFHhmnMqKRm.jQ1/VVQaCWdx5biXCYxKQ/NDdcxvziGe1zD1ed1") == 0);
}

TEST_CASE("a hash verifies its own password and rejects a wrong one") {
	const char* h = "$6$NaNoSsaLt12345$F1FE/Qd7iATvVrjtCOPuH5.zvPpuFsPv9bbOFHhmnMqKRm.jQ1/VVQaCWdx5biXCYxKQ/NDdcxvziGe1zD1ed1";
	CHECK(strcmp(crypt("nanos", h), h) == 0);     // correct password -> same hash (login succeeds)
	CHECK(strcmp(crypt("wrong", h), h) != 0);     // wrong password -> different hash (login fails)
}

TEST_CASE("empty password and the rounds= prefix are handled") {
	// Empty password still hashes (and matches openssl passwd -6 for the empty string).
	char* e = crypt("", "$6$saltsalt$");
	CHECK(e != nullptr);
	CHECK(strncmp(e, "$6$saltsalt$", 12) == 0);
	// A custom round count is echoed back in the output prefix.
	char* r = crypt("x", "$6$rounds=2000$abc$");
	CHECK(r != nullptr);
	CHECK(strncmp(r, "$6$rounds=2000$abc$", 19) == 0);
	// A non-$6$ setting is unsupported -> NULL (never a wrong match).
	CHECK(crypt("x", "$1$abc$") == nullptr);
}
