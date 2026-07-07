/*
 * linuxkpi/kpi_idr.c — idr/ida over a growable id→pointer array.
 *
 * Not the radix-tree Linux uses; a flat array is fine for the handful of resource handles
 * a GPU/virtio driver allocates. Allocation scans for the lowest free slot >= base/start,
 * growing the array as needed. ida stores a non-NULL sentinel so id 0 is allocatable.
 */
#include <linux/idr.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/printk.h>

#define IDA_SENTINEL ((void *)1)

void idr_init_base(struct idr *idr, int base) {
	idr->slots = 0;
	idr->cap = 0;
	idr->base = base;
}

void idr_init(struct idr *idr) { idr_init_base(idr, 0); }

void idr_destroy(struct idr *idr) {
	kfree(idr->slots);
	idr->slots = 0;
	idr->cap = 0;
}

static int idr_grow(struct idr *idr, int need) {
	if (need <= idr->cap)
		return 0;
	int ncap = idr->cap ? idr->cap * 2 : 8;
	while (ncap < need)
		ncap *= 2;
	void **ns = (void **)krealloc(idr->slots, (size_t)ncap * sizeof(void *), 0);
	if (!ns)
		return -1;
	for (int i = idr->cap; i < ncap; i++)
		ns[i] = 0;
	idr->slots = ns;
	idr->cap = ncap;
	return 0;
}

int idr_alloc(struct idr *idr, void *ptr, int start, int end, gfp_t gfp) {
	(void)gfp;
	int lo = start > idr->base ? start : idr->base;
	for (int id = lo; (end <= 0) || id < end; id++) {
		if (id >= idr->cap) {
			if (idr_grow(idr, id + 1) < 0)
				return -12 /* -ENOMEM */;
		}
		if (!idr->slots[id]) {
			idr->slots[id] = ptr ? ptr : IDA_SENTINEL;
			return id;
		}
	}
	return -28 /* -ENOSPC */;
}

void *idr_find(struct idr *idr, int id) {
	if (id < 0 || id >= idr->cap)
		return 0;
	void *p = idr->slots[id];
	return (p == IDA_SENTINEL) ? 0 : p;
}

void *idr_remove(struct idr *idr, int id) {
	if (id < 0 || id >= idr->cap)
		return 0;
	void *p = idr->slots[id];
	idr->slots[id] = 0;
	return (p == IDA_SENTINEL) ? 0 : p;
}

/* xa_store's backend: INSERT-or-replace at a fixed id — real xarray semantics. This is NOT
 * idr_replace, which only swaps an already-allocated slot and silently DROPS a store to a
 * fresh id (that mismatch cost a Dell boot: i915_gem_context_open registers the default
 * proto-context via xa_store(&proto_context_xa, 0, pc) on a virgin xarray, the entry
 * vanished, and every EXECBUFFER2 died with -ENOENT in i915_gem_context_lookup). */
void *idr_store(struct idr *idr, void *ptr, int id) {
	void *old;
	if (id < 0)
		return 0;
	if (id >= idr->cap && idr_grow(idr, id + 1) < 0) {
		printk("lkpi idr: idr_store grow FAILED id=%d — entry dropped\n", id);
		return 0;
	}
	old = idr->slots[id];
	idr->slots[id] = ptr ? ptr : IDA_SENTINEL;
	return (old == IDA_SENTINEL) ? 0 : old;
}

void *idr_replace(struct idr *idr, void *ptr, int id) {
	void *old = 0;
	if (id >= 0 && id < idr->cap && idr->slots[id]) {
		old = idr->slots[id];
		idr->slots[id] = ptr ? ptr : IDA_SENTINEL;
	}
	return (old == IDA_SENTINEL) ? 0 : old;
}

int idr_is_empty(struct idr *idr) {
	for (int i = 0; i < idr->cap; i++)
		if (idr->slots[i])
			return 0;
	return 1;
}

/* Call fn(id, ptr, data) for each populated slot; stop (and return) on first non-zero. */
int idr_for_each(struct idr *idr, int (*fn)(int, void *, void *), void *data) {
	for (int i = 0; i < idr->cap; i++) {
		void *p = idr->slots[i];
		if (p) {
			int r = fn(idr->base + i, (p == IDA_SENTINEL) ? 0 : p, data);
			if (r)
				return r;
		}
	}
	return 0;
}

/* ---- ida: store the sentinel so any id (incl 0) is valid ---- */
void ida_init(struct ida *ida) { idr_init(&ida->idr); }
void ida_destroy(struct ida *ida) { idr_destroy(&ida->idr); }

int ida_alloc_range(struct ida *ida, unsigned int min, unsigned int max, gfp_t gfp) {
	int end = (max == 0xffffffffu) ? 0 : (int)max + 1;
	return idr_alloc(&ida->idr, IDA_SENTINEL, (int)min, end, gfp);
}

int ida_alloc(struct ida *ida, gfp_t gfp) {
	return ida_alloc_range(ida, 0, 0xffffffffu, gfp);
}

void ida_free(struct ida *ida, unsigned int id) {
	idr_remove(&ida->idr, (int)id);
}
