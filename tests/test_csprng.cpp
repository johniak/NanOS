/*
 * test_csprng.cpp — the kernel CSPRNG (kernel/Csprng.*).
 *
 * Three things must hold for the crypto stack built on top (TLS/SSH) to be sound:
 *   1. the ChaCha20 core is correct       -> RFC 8439 §2.4.2 known-answer vector
 *   2. different seeds give different streams, and a seed is reproducible -> determinism
 *   3. the output looks random            -> monobit + no immediate repetition sanity
 * The class is deterministic given its seed (so it is testable here); the *kernel* instance is
 * seeded from real entropy (RDRAND+jitter+RTC) at boot, which is what makes it unpredictable.
 */
#include "doctest.h"
#include "Csprng.h"
#include <cstring>

using namespace kernel;

TEST_CASE("chacha20 block matches the RFC 8439 known-answer vector") {
	// RFC 8439 §2.4.2: key = 00..1f, nonce = 00:00:00:00:00:00:00:4a:00:00:00:00, counter = 1.
	// (The test uses the §2.4 nonce 00000000 0000004a 00000000.)
	unsigned key[8] = {
		0x03020100u, 0x07060504u, 0x0b0a0908u, 0x0f0e0d0cu,
		0x13121110u, 0x17161514u, 0x1b1a1918u, 0x1f1e1d1cu,
	};
	unsigned nonce[3] = { 0x00000000u, 0x4a000000u, 0x00000000u };
	unsigned counter = 1;
	unsigned char out[64];
	chacha20Block(key, counter, nonce, out);

	// Expected keystream block 1 (RFC 8439 §2.4.2).
	const unsigned char expect[64] = {
		0x22,0x4f,0x51,0xf3,0x40,0x1b,0xd9,0xe1,0x2f,0xde,0x27,0x6f,0xb8,0x63,0x1d,0xed,
		0x8c,0x13,0x1f,0x82,0x3d,0x2c,0x06,0xe2,0x7e,0x4f,0xca,0xec,0x9e,0xf3,0xcf,0x78,
		0x8a,0x3b,0x0a,0xa3,0x72,0x60,0x0a,0x92,0xb5,0x79,0x74,0xcd,0xed,0x2b,0x93,0x34,
		0x79,0x4c,0xba,0x40,0xc6,0x3e,0x34,0xcd,0xea,0x21,0x2c,0x4c,0xf0,0x7d,0x41,0xb7,
	};
	CHECK(memcmp(out, expect, 64) == 0);
}

TEST_CASE("same seed reproduces the same stream; different seed diverges") {
	unsigned char seedA[16];
	for (int i = 0; i < 16; i++) seedA[i] = (unsigned char) (i * 7 + 1);
	unsigned char seedB[16];
	memcpy(seedB, seedA, 16);
	seedB[0] ^= 0x01;                       // a single-bit difference

	Csprng a1, a2, b;
	a1.seed(seedA, 16);
	a2.seed(seedA, 16);
	b.seed(seedB, 16);

	unsigned char xa1[256], xa2[256], xb[256];
	a1.bytes(xa1, sizeof xa1);
	a2.bytes(xa2, sizeof xa2);
	b.bytes(xb, sizeof xb);

	CHECK(memcmp(xa1, xa2, sizeof xa1) == 0);   // identical seed -> identical stream (reproducible)
	CHECK(memcmp(xa1, xb, sizeof xa1) != 0);    // one-bit seed change -> a different stream
}

TEST_CASE("output passes a monobit + chunk-uniqueness sanity check") {
	unsigned char seed[32];
	for (int i = 0; i < 32; i++) seed[i] = (unsigned char) (0xa5 ^ (i * 13));
	Csprng g;
	g.seed(seed, sizeof seed);

	// Monobit: over 8 KiB the count of 1-bits should sit near 50% (32768 of 65536). A broken
	// generator (stuck/constant/biased) falls far outside a generous ±5% window.
	unsigned char buf[8192];
	g.bytes(buf, sizeof buf);
	long ones = 0;
	for (unsigned i = 0; i < sizeof buf; i++)
		for (int b = 0; b < 8; b++)
			ones += (buf[i] >> b) & 1;
	long total = (long) sizeof buf * 8;        // 65536 bits
	CHECK(ones > total * 45 / 100);
	CHECK(ones < total * 55 / 100);

	// No two consecutive 16-byte chunks are equal (a keystream that repeats a block is broken).
	for (unsigned i = 16; i < sizeof buf; i += 16)
		CHECK(memcmp(buf + i - 16, buf + i, 16) != 0);
}

TEST_CASE("shared kernel instance: seed/reseed/bytes + boot seeding produce output") {
	// Exercise the free-function wrappers around the shared instance and the boot seeder. Under
	// the host harness archHwRandom() returns false and archEntropyTick() returns 0, so this
	// drives the no-RDRAND path: the jitter loop folds the (constant) ticks and the RTC into the
	// pool. We can't assert non-determinism here (the host stubs are fixed) — that is the QEMU
	// gate (step 0.5) — but we verify the path runs and yields bytes that advance.
	csprngKernelSeed();
	unsigned char a[64], b[64];
	csprngBytes(a, sizeof a);
	csprngBytes(b, sizeof b);
	CHECK(memcmp(a, b, sizeof a) != 0);          // successive draws differ (stream advances)

	unsigned char extra[16];
	for (int i = 0; i < 16; i++) extra[i] = (unsigned char) (i * 3 + 2);
	csprngReseed(extra, sizeof extra);           // mix more entropy into the shared instance
	unsigned char c[64];
	csprngBytes(c, sizeof c);
	CHECK(memcmp(b, c, sizeof b) != 0);

	csprngSeed(extra, sizeof extra);             // a fresh explicit seed
	unsigned char d[64];
	csprngBytes(d, sizeof d);
	CHECK(memcmp(c, d, sizeof c) != 0);
}

TEST_CASE("a zero-length seed leaves the generator usable (no divide/modulo by zero)") {
	Csprng g;
	g.seed(0, 0);                                // len 0: the mixing loop runs zero times
	CHECK(g.seeded());
	unsigned char x[32];
	g.bytes(x, sizeof x);                        // must still produce bytes
	// Two draws differ (the stream advances even from an all-zero key).
	unsigned char y[32];
	g.bytes(y, sizeof y);
	CHECK(memcmp(x, y, sizeof x) != 0);
}

TEST_CASE("reseed mixes in entropy without resetting the stream to a fixed value") {
	Csprng g;
	unsigned char s0[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	g.seed(s0, sizeof s0);
	unsigned char before[64];
	g.bytes(before, sizeof before);

	unsigned char extra[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };
	g.reseed(extra, sizeof extra);
	unsigned char after[64];
	g.bytes(after, sizeof after);
	CHECK(memcmp(before, after, 64) != 0);     // reseed perturbs the stream
}
