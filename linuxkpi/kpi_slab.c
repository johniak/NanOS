/*
 * linuxkpi/kpi_slab.c — kmalloc family over the NanOS kernel heap.
 *
 * The kernel heap (knx_malloc/knx_free) has no size query and no zeroing, so each block
 * is laid out as [ size_t length | 16 bytes padding to keep payload 16-aligned | payload ].
 * The header lets krealloc copy the right number of bytes and ksize answer truthfully.
 *
 * In host doctest builds, knx_malloc/knx_free are provided by a test shim forwarding to
 * libc (mirroring tests/host_shims.cpp for the C++ allocator).
 */
#include <linux/slab.h>
#include <linux/string.h>

extern void *knx_malloc(unsigned size);
extern void  knx_free(void *p);

#define SLAB_HDR 16u  /* room for a size_t, padded so payload stays 16-byte aligned */

void *kmalloc(size_t size, gfp_t flags) {
	unsigned char *base = (unsigned char *)knx_malloc((unsigned)(size + SLAB_HDR));
	if (!base)
		return 0;
	*(size_t *)base = size;
	void *payload = base + SLAB_HDR;
	if (flags & __GFP_ZERO)
		memset(payload, 0, size);
	return payload;
}

void *kzalloc(size_t size, gfp_t flags) {
	return kmalloc(size, flags | __GFP_ZERO);
}

void *kmalloc_array(size_t n, size_t size, gfp_t flags) {
	return kmalloc(n * size, flags);
}

void *kcalloc(size_t n, size_t size, gfp_t flags) {
	return kzalloc(n * size, flags);
}

size_t ksize(const void *p) {
	if (!p)
		return 0;
	return *(const size_t *)((const unsigned char *)p - SLAB_HDR);
}

void kfree(const void *p) {
	if (!p)
		return;
	knx_free((unsigned char *)p - SLAB_HDR);
}

void *krealloc(const void *p, size_t new_size, gfp_t flags) {
	if (!p)
		return kmalloc(new_size, flags);
	if (new_size == 0) {
		kfree(p);
		return 0;
	}
	size_t old = ksize(p);
	void *np = kmalloc(new_size, flags);
	if (!np)
		return 0;
	memcpy(np, p, old < new_size ? old : new_size);
	kfree(p);
	return np;
}

/* vmalloc/kvmalloc collapse onto the same contiguous heap for the shim. */
void *kvmalloc(size_t size, gfp_t flags) { return kmalloc(size, flags); }
void *kvzalloc(size_t size, gfp_t flags) { return kzalloc(size, flags); }
void  kvfree(const void *p)              { kfree(p); }
void *vmalloc(size_t size)               { return kmalloc(size, 0); }
void *vzalloc(size_t size)               { return kzalloc(size, __GFP_ZERO); }
void  vfree(const void *p)               { kfree(p); }

char *kstrdup(const char *s, gfp_t flags) {
	if (!s)
		return 0;
	size_t n = strlen(s) + 1;
	char *d = (char *)kmalloc(n, flags);
	if (d)
		memcpy(d, s, n);
	return d;
}
