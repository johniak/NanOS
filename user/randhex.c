/*
 * randhex — print cryptographic random bytes as hex (a tiny `od`-for-entropy).
 *
 * Doubles as the FAZA 0 proof tool: it draws from BOTH kernel entropy channels — getentropy(3)
 * (which goes through getrandom(2)) and /dev/urandom — and prints each as hex. Because the kernel
 * CSPRNG is now seeded from real entropy (RDRAND + RDTSC jitter + RTC) instead of a fixed seed, the
 * lines printed by two separate boots DIFFER — that is the non-determinism gate. Usage:
 *   randhex [nbytes]      (default 16, capped at 256 to match getentropy's limit)
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int getentropy(void* buf, size_t n);

static void hexline(const char* label, const unsigned char* b, int n) {
	printf("%s", label);
	for (int i = 0; i < n; i++)
		printf("%02x", b[i]);
	printf("\n");
}

int main(int argc, char** argv) {
	int n = (argc > 1) ? atoi(argv[1]) : 16;
	if (n <= 0) n = 16;
	if (n > 256) n = 256;

	unsigned char buf[256];

	/* 1) getentropy -> getrandom(2) -> kernel CSPRNG */
	if (getentropy(buf, (size_t) n) == 0)
		hexline("getentropy: ", buf, n);
	else
		printf("getentropy: FAIL\n");

	/* 2) /dev/urandom -> the same kernel CSPRNG */
	FILE* f = fopen("/dev/urandom", "rb");
	if (f) {
		size_t got = fread(buf, 1, (size_t) n, f);
		fclose(f);
		hexline("urandom:    ", buf, (int) got);
	} else {
		printf("urandom:    FAIL open\n");
	}
	return 0;
}
