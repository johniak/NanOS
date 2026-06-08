/*
 * brktest — stress the growable brk/sbrk heap in QEMU.
 *
 * Doom needs a single malloc(6 MiB) for its zone allocator, far larger than the old
 * fixed 448 KiB window. This allocates 6 MiB, writes a checkable pattern across every
 * page, reads it back, frees it, and reports — proving the high-VA heap maps, is
 * writable, and tears down. (Stage 2 of the Doom port.)
 */
#include <stdio.h>
#include <stdlib.h>

#define SZ (6 * 1024 * 1024)

int main(void) {
	printf("brktest: requesting %d bytes via malloc...\n", SZ);
	unsigned char* p = (unsigned char*) malloc(SZ);
	if (!p) {
		printf("brktest: malloc FAILED\n");
		return 1;
	}
	printf("brktest: got block at %p, writing pattern...\n", (void*) p);

	/* Touch one byte per 4 KiB page so every mapped page is exercised. */
	for (int i = 0; i < SZ; i += 4096)
		p[i] = (unsigned char) ((i / 4096) & 0xFF);

	int bad = 0;
	for (int i = 0; i < SZ; i += 4096)
		if (p[i] != (unsigned char) ((i / 4096) & 0xFF))
			bad++;

	printf("brktest: verified %d pages, %d mismatches\n", SZ / 4096, bad);
	free(p);
	printf("brktest: %s\n", bad ? "FAIL" : "OK (6 MiB heap works)");
	return bad ? 1 : 0;
}
