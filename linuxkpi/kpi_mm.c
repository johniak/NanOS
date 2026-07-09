/*
 * linuxkpi/kpi_mm.c — page-aligned allocations for the shim.
 *
 * NanOS RAM is identity-mapped (virt == phys), so page memory from the kernel heap is
 * directly DMA-addressable and virt_to_phys() (linux/mm.h) is the identity. We over-
 * allocate and align up, stashing the original heap pointer just below the aligned block
 * so free can recover it.
 */
#include <linux/mm.h>
#include <linux/string.h>
#include "lkpi_knx.h"

#define PG_ALIGN 4096UL

/*
 * mem_map: one struct page per physical page frame, so a struct page* can carry per-page metadata
 * (flags/refcount/private/lru) SEPARATE from the page's data — the real Linux model, required by
 * TTM and the rest of i915. Sized from knx_ram_top() (highest usable phys address) at kext load;
 * ~1.5% of RAM, like Linux. page_to_pfn(p) = p - lkpi_mem_map (see linux/mm.h).
 */
struct page *lkpi_mem_map = 0;
unsigned long lkpi_mem_map_pfns = 0;

int lkpi_mem_map_init(void) {
	if (lkpi_mem_map)
		return 1;   /* idempotent */
	unsigned long long top = knx_ram_top();
	unsigned long pfns = (unsigned long)(top >> PAGE_SHIFT) + 1;
	unsigned long bytes = pfns * sizeof(struct page);
	struct page *map = (struct page *)knx_malloc((unsigned)bytes);
	if (!map) {
		knx_log("lkpi: FATAL mem_map alloc failed\n");
		return 0;   /* leaves lkpi_mem_map NULL; caller MUST abort (a page access would fault) */
	}
	memset(map, 0, bytes);
	lkpi_mem_map_pfns = pfns;
	lkpi_mem_map = map;   /* publish last: any concurrent reader sees a fully-zeroed map */
	return 1;
}

/* Big blocks (GEM buffer objects: a 1080p texture is ~8 MiB, a desktop holds dozens) come from
 * the FRAME POOL, not the byte heap: the heap is capped at 512 MiB and the mem_map alone eats a
 * large slice of it, so heap-backed GEM starved on the Dell (boot #49: every glTexImage2D died
 * GL_OUT_OF_MEMORY with zero kernel evidence). Frame-pool blocks carry a header PAGE (magic +
 * base + size) below the returned pointer so free_pages_exact can route to the right allocator;
 * the heap path keeps its old single-word stash. Threshold 64 KiB: small metadata allocations
 * stay on the heap (cheap, abundant), everything texture-sized goes to frames. min_pa 0x60000000
 * keeps blocks identity-visible under process CR3 (above every privatized user VA window); on
 * low-RAM machines (QEMU -m 512) there are simply no frames up there and we fall back to the
 * heap, which is exactly the pre-frame-pool behaviour. */
#define FRAMES_MIN_BYTES (64UL * 1024)
#define FRAMES_MIN_PA    0x60000000ULL
#define FRAMES_MAGIC     0x4c4b5049464d4dULL   /* "LKPIFMM" */

void *alloc_pages_exact(size_t size, gfp_t gfp) {
	if (size == 0)
		size = 1;
	if (size >= FRAMES_MIN_BYTES) {
		unsigned long long pa = knx_alloc_frames((unsigned long long)size + PG_ALIGN,
							 FRAMES_MIN_PA);
		if (pa) {
			/* header page: [-1] = raw base (mirrors the heap stash), [-2] = magic */
			unsigned long aligned = (unsigned long)pa + PG_ALIGN;
			((void **)aligned)[-1] = (void *)(unsigned long)pa;
			((unsigned long long *)aligned)[-2] = FRAMES_MAGIC;
			if (gfp & __GFP_ZERO)
				memset((void *)aligned, 0, size);
			return (void *)aligned;
		}
		/* no contiguous frame run (low-RAM QEMU, or fragmentation) — fall through to the heap */
	}
	/* room to align up to a page boundary + a slot for the base pointer */
	unsigned long raw = (unsigned long)knx_malloc((unsigned)(size + PG_ALIGN + sizeof(void *)));
	if (!raw) {
		extern int printk(const char*,...);
		printk("lkpi: alloc_pages_exact OOM size=%lu heap free=%llu/%llu\n",
		       (unsigned long)size, knx_heap_free(), knx_heap_total());
		return 0;
	}
	unsigned long aligned = (raw + sizeof(void *) + (PG_ALIGN - 1)) & ~(PG_ALIGN - 1);
	((void **)aligned)[-1] = (void *)raw;
	if (gfp & __GFP_ZERO) {
		unsigned char *p = (unsigned char *)aligned;
		for (size_t i = 0; i < size; i++)
			p[i] = 0;
	}
	return (void *)aligned;
}

void free_pages_exact(void *virt, size_t size) {
	if (!virt)
		return;
	/* frame-pool block? (magic in the header page, base one page below) */
	if (((unsigned long long *)virt)[-2] == FRAMES_MAGIC &&
	    ((void **)virt)[-1] == (void *)((unsigned long)virt - PG_ALIGN)) {
		((unsigned long long *)virt)[-2] = 0;   /* poison: a double free must not re-match */
		knx_free_frames((unsigned long)virt - PG_ALIGN,
				(unsigned long long)size + PG_ALIGN);
		return;
	}
	void *base = ((void **)virt)[-1];
	/* Guard: a valid alloc_pages_exact block stashes its raw base in the word just below the
	 * aligned pointer, and that base is always < virt and non-NULL. If it isn't, `virt` is an
	 * INTERIOR address (e.g. a slice of a contiguous shmem block, whose only header sits at the
	 * base) — freeing it would hand knx_free() garbage and corrupt the heap. Skip + log loudly. */
	if (!base || (unsigned long)base >= (unsigned long)virt ||
	    (unsigned long)virt - (unsigned long)base > 4096UL + sizeof(void *)) {
		extern int printk(const char*,...);
		printk("lkpi: free_pages_exact SKIP interior/bad virt=%lx base=%lx\n",
		       (unsigned long)virt, (unsigned long)base);
		return;
	}
	knx_free(base);
}
