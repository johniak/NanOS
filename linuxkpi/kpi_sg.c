/*
 * linuxkpi/kpi_sg.c — scatter/gather table allocation for the shim.
 *
 * Implements the flat (non-chained) sg_table API declared in <linux/scatterlist.h>:
 * a table is a kmalloc'd array of `nents` scatterlist entries, the last marked SG_END.
 * On the identity-mapped NanOS kernel a struct page* IS the page's physical/virtual
 * address (see <linux/mm.h>), so building entries from a page array is a direct copy;
 * dma_map_sgtable() later fills dma_address from sg_phys().
 *
 * This is the surface drm_gem_shmem uses to describe a GEM object's backing pages to
 * the device (virtio-gpu ATTACH_BACKING / mem entries) — the display hot path.
 */
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/mm.h>

int sg_alloc_table(struct sg_table *t, unsigned int nents, unsigned gfp)
{
	if (!t || !nents)
		return -EINVAL;
	t->sgl = (struct scatterlist *)kmalloc(sizeof(struct scatterlist) * nents, gfp);
	if (!t->sgl)
		return -ENOMEM;
	sg_init_table(t->sgl, nents);
	t->nents = nents;
	t->orig_nents = nents;
	return 0;
}

void sg_free_table(struct sg_table *t)
{
	if (t && t->sgl) {
		kfree(t->sgl);
		t->sgl = 0;
		t->nents = t->orig_nents = 0;
	}
}

/*
 * Build a table describing [pages, n_pages) starting at byte `offset`, total `size`
 * bytes, coalescing physically-contiguous pages into one entry (capped at max_seg).
 * Returns 0 on success. Matches the kernel signature drm_gem_shmem expects.
 */
int sg_alloc_table_from_pages_segment(struct sg_table *sgt, struct page **pages,
				      unsigned n_pages, unsigned offset,
				      unsigned long size, unsigned max_seg, unsigned gfp)
{
	unsigned int i, segs = 0;
	struct scatterlist *sg;
	unsigned long left;

	if (!sgt || !pages || !n_pages || !size)
		return -EINVAL;
	if (max_seg == 0)
		max_seg = (unsigned)-1;
	max_seg &= ~(unsigned)(PAGE_SIZE - 1);
	if (max_seg == 0)
		max_seg = PAGE_SIZE;

	/* First pass: count coalesced segments. */
	{
		unsigned long prev_end = 0;
		unsigned int cur = 0;
		for (i = 0; i < n_pages; i++) {
			unsigned long pa = (unsigned long)pages[i];
			if (i > 0 && pa == prev_end && cur < max_seg) {
				cur += PAGE_SIZE;
			} else {
				if (i > 0) segs++;
				cur = PAGE_SIZE;
			}
			prev_end = pa + PAGE_SIZE;
		}
		segs++;
	}

	if (sg_alloc_table(sgt, segs, gfp))
		return -ENOMEM;

	/* Second pass: emit coalesced entries, honoring offset (first) and size (last). */
	sg = sgt->sgl;
	left = size;
	{
		unsigned long seg_base = (unsigned long)pages[0];
		unsigned long seg_len = 0;
		unsigned int off = offset;
		for (i = 0; i < n_pages; i++) {
			unsigned long pa = (unsigned long)pages[i];
			if (i > 0 && pa == seg_base + seg_len && seg_len < max_seg) {
				seg_len += PAGE_SIZE;
			} else if (i > 0) {
				unsigned int this_off = off; unsigned long this_len = seg_len - this_off;
				if (this_len > left) this_len = left;
				sg_set_page(sg, (struct page *)seg_base, (unsigned)this_len, this_off);
				left -= this_len; off = 0; sg = sg_next(sg);
				seg_base = pa; seg_len = PAGE_SIZE;
			} else {
				seg_base = pa; seg_len = PAGE_SIZE;
			}
		}
		/* final segment */
		{
			unsigned int this_off = off; unsigned long this_len = seg_len - this_off;
			if (this_len > left) this_len = left;
			sg_set_page(sg, (struct page *)seg_base, (unsigned)this_len, this_off);
		}
	}
	return 0;
}
