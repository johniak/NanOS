/*
 * malloctest — assert the C allocator returns 16-byte-aligned memory (System V AMD64 ABI).
 *
 * picolibc's nano-malloc derived its alignment from a union {void*,double,long long,size_t}
 * (alignof 8 on x86_64), so malloc/calloc/realloc returned only 8-byte-aligned blocks. That
 * violates the ABI's max_align_t (16) requirement: any 16-byte-aligned SSE store into malloc'd
 * memory (e.g. Mesa ralloc's `movaps %xmm0,(%rax)` into an alignas(16) struct) #GPs. We backported
 * picolibc's upstream fix (widen the alignment union with `long double`, alignof 16 here) so this
 * program must now see every pointer 16-aligned. It is a permanent regression gate (smoke-x86_64):
 * the change is in libc, so it affects EVERY NanOS program, and must never silently regress.
 *
 * Prints exactly one terminal marker: "malloctest: MALLOC_ALIGN16 PASS" or "... FAIL ...".
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define ALIGNED(p) (((uintptr_t)(void *)(p) & 15u) == 0)

static int fail(const char *what, size_t n, void *p) {
	printf("malloctest: MALLOC_ALIGN16 FAIL (%s n=%zu ptr=%p)\n", what, n, p);
	return 1;
}

int main(void) {
	/* Every small size 1..256 plus a spread of larger ones that force fresh sbrk chunks. */
	static const size_t big[] = { 511, 512, 1000, 4096, 65536, 1u << 20 };

	for (size_t n = 1; n <= 256; n++) {
		void *p = malloc(n);
		if (!p || !ALIGNED(p)) return fail("malloc", n, p);
		free(p);

		void *c = calloc(1, n);
		if (!c || !ALIGNED(c)) return fail("calloc", n, c);
		free(c);
	}

	for (size_t i = 0; i < sizeof big / sizeof big[0]; i++) {
		size_t n = big[i];
		void *p = malloc(n);
		if (!p || !ALIGNED(p)) return fail("malloc-big", n, p);
		/* grow then shrink in place; realloc must keep the result 16-aligned either way. */
		void *g = realloc(p, n * 2);
		if (!g || !ALIGNED(g)) return fail("realloc-grow", n * 2, g);
		void *s = realloc(g, 32);
		if (!s || !ALIGNED(s)) return fail("realloc-shrink", 32, s);
		free(s);
	}

	printf("malloctest: MALLOC_ALIGN16 PASS\n");
	return 0;
}
