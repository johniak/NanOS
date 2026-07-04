/*
 * linuxkpi/kpi_sort.c — heapsort + bsearch for the LinuxKPI shim.
 *
 * sort() is heapsort (O(n log n), in place, no recursion — safe for a ring-0 stack).
 * An optional swap callback handles element-specific swapping; otherwise bytes are
 * swapped directly. bsearch collides with libc in host-test builds, so the engine is
 * exposed as lkpi_bsearch and the libc-named alias is compiled only for the kext.
 */
#include <linux/sort.h>

static void byteswap(char *a, char *b, int size) {
	for (int i = 0; i < size; i++) {
		char t = a[i];
		a[i] = b[i];
		b[i] = t;
	}
}

static void do_swap(char *a, char *b, int size, void (*swapf)(void *, void *, int)) {
	if (swapf)
		swapf(a, b, size);
	else
		byteswap(a, b, size);
}

/* sift down element at `start` within heap of `n` elements. */
static void siftdown(char *base, size_t n, size_t start, size_t size,
                     int (*cmp)(const void *, const void *),
                     void (*swapf)(void *, void *, int)) {
	size_t root = start;
	for (;;) {
		size_t child = 2 * root + 1;
		if (child >= n)
			break;
		if (child + 1 < n &&
		    cmp(base + child * size, base + (child + 1) * size) < 0)
			child++;
		if (cmp(base + root * size, base + child * size) < 0) {
			do_swap(base + root * size, base + child * size, (int)size, swapf);
			root = child;
		} else {
			break;
		}
	}
}

void sort(void *vbase, size_t num, size_t size,
          int (*cmp)(const void *, const void *),
          void (*swapf)(void *, void *, int)) {
	char *base = (char *)vbase;
	if (num < 2 || size == 0)
		return;
	/* build a max-heap */
	for (size_t i = num / 2; i-- > 0;)
		siftdown(base, num, i, size, cmp, swapf);
	/* pop max to the end, shrink heap */
	for (size_t end = num - 1; end > 0; end--) {
		do_swap(base, base + end * size, (int)size, swapf);
		siftdown(base, end, 0, size, cmp, swapf);
	}
}

/* sort_r: same heap-free contract as sort() but the comparator/swap receive a caller `priv`.
 * Insertion sort — i915's sort_r inputs are tiny (a handful of VBT/engine entries), so O(n^2) is
 * fine and keeps the priv-threading trivially correct. */
void sort_r(void *vbase, size_t num, size_t size,
            int (*cmp)(const void *, const void *, const void *priv),
            void (*swapf)(void *, void *, int),
            const void *priv) {
	char *base = (char *)vbase;
	if (num < 2 || size == 0)
		return;
	for (size_t i = 1; i < num; i++)
		for (size_t j = i; j > 0 && cmp(base + (j - 1) * size, base + j * size, priv) > 0; j--)
			do_swap(base + (j - 1) * size, base + j * size, (int)size, swapf);
}

void *lkpi_bsearch(const void *key, const void *base, size_t num, size_t size,
                   int (*cmp)(const void *, const void *)) {
	const char *b = (const char *)base;
	size_t lo = 0, hi = num;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int c = cmp(key, b + mid * size);
		if (c == 0)
			return (void *)(b + mid * size);
		if (c < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	return 0;
}

#ifndef NANOS_HOST_TEST
void *bsearch(const void *key, const void *base, size_t num, size_t size,
              int (*cmp)(const void *, const void *)) {
	return lkpi_bsearch(key, base, num, size, cmp);
}
#endif
