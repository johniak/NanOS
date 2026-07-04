/*
 * linuxkpi/include/linux/scatterlist.h — scatter/gather lists for the LinuxKPI shim.
 *
 * A flat (non-chained) subset sufficient for virtio_ring. The buffer's kernel virtual
 * address is stored in page_link with the two low bits reserved for the chain/end flags
 * (our kmalloc payloads are >=16-byte aligned, so the low bits are free). On an
 * identity-mapped kernel, virt == phys == dma_address.
 */
#ifndef _LINUXKPI_LINUX_SCATTERLIST_H
#define _LINUXKPI_LINUX_SCATTERLIST_H

#include <linux/types.h>
#include <linux/string.h>

struct page;  /* token: pointer value == kernel virtual address (see linux/mm.h) */

struct scatterlist {
	unsigned long  page_link;     /* buffer addr | flags (bit0 chain, bit1 end) */
	unsigned int   offset;
	unsigned int   length;
	dma_addr_t     dma_address;
	unsigned int   dma_length;
};

#define SG_CHAIN  0x1UL
#define SG_END    0x2UL
#define SG_PAGE_LINK_MASK (~(SG_CHAIN | SG_END))

static inline void sg_assign_buf(struct scatterlist *sg, const void *buf) {
	unsigned long flags = sg->page_link & (SG_CHAIN | SG_END);
	sg->page_link = ((unsigned long)buf & SG_PAGE_LINK_MASK) | flags;
}

static inline void *sg_virt(struct scatterlist *sg) {
	return (void *)((sg->page_link & SG_PAGE_LINK_MASK) + sg->offset);
}

/* page model: a struct page* is the page's kernel virtual address (see linux/mm.h). */
static inline struct page *sg_page(struct scatterlist *sg) {
	return (struct page *)(sg->page_link & SG_PAGE_LINK_MASK);
}
static inline void sg_set_page(struct scatterlist *sg, struct page *page,
                               unsigned int len, unsigned int offset) {
	unsigned long flags = sg->page_link & (SG_CHAIN | SG_END);
	sg->page_link = ((unsigned long)page & SG_PAGE_LINK_MASK) | flags;
	sg->offset = offset;
	sg->length = len;
}
static inline void sg_assign_page(struct scatterlist *sg, struct page *page) {
	unsigned long flags = sg->page_link & (SG_CHAIN | SG_END);
	sg->page_link = ((unsigned long)page & SG_PAGE_LINK_MASK) | flags;
}

static inline int sg_is_chain(struct scatterlist *sg) { return !!(sg->page_link & SG_CHAIN); }
static inline int sg_is_last(struct scatterlist *sg)  { return !!(sg->page_link & SG_END); }
static inline struct scatterlist *sg_chain_ptr(struct scatterlist *sg)
{ return (struct scatterlist *)(sg->page_link & ~(SG_CHAIN | SG_END)); }

static inline void sg_mark_end(struct scatterlist *sg) {
	sg->page_link |= SG_END;
	sg->page_link &= ~SG_CHAIN;
}
static inline void sg_unmark_end(struct scatterlist *sg) { sg->page_link &= ~SG_END; }

static inline struct scatterlist *sg_next(struct scatterlist *sg) {
	if (sg_is_last(sg))
		return 0;
	return sg + 1;
}

static inline void sg_init_table(struct scatterlist *sgl, unsigned int nents) {
	memset(sgl, 0, sizeof(*sgl) * nents);
	sg_mark_end(&sgl[nents - 1]);
}

static inline void sg_set_buf(struct scatterlist *sg, const void *buf, unsigned int buflen) {
	sg_assign_buf(sg, buf);
	sg->offset = 0;
	sg->length = buflen;
}

static inline void sg_init_one(struct scatterlist *sg, const void *buf, unsigned int buflen) {
	sg_init_table(sg, 1);
	sg_set_buf(sg, buf, buflen);
}

static inline dma_addr_t sg_phys(struct scatterlist *sg) {
	return (dma_addr_t)(sg->page_link & SG_PAGE_LINK_MASK) + sg->offset;
}

#define sg_dma_address(sg) ((sg)->dma_address)
#define sg_dma_len(sg)     ((sg)->dma_length)

#define for_each_sg(sglist, sg, nr, i) \
	for ((i) = 0, (sg) = (sglist); (i) < (nr); (i)++, (sg) = sg_next(sg))

struct sg_table { struct scatterlist *sgl; unsigned int nents; unsigned int orig_nents; };

#ifdef __cplusplus
extern "C" {
#endif
int  sg_alloc_table(struct sg_table *t, unsigned int nents, unsigned gfp);
void sg_free_table(struct sg_table *t);
#ifdef __cplusplus
}
#endif

#define for_each_sgtable_sg(sgt, sg, i)     for_each_sg((sgt)->sgl, sg, (sgt)->orig_nents, i)
#define for_each_sgtable_dma_sg(sgt, sg, i) for_each_sg((sgt)->sgl, sg, (sgt)->nents, i)

#endif /* _LINUXKPI_LINUX_SCATTERLIST_H */

#ifndef _LKPI_SG_PAGEITER
#define _LKPI_SG_PAGEITER
struct sg_page_iter { struct scatterlist *sg; unsigned int sg_pgoffset; };
struct sg_dma_page_iter { struct sg_page_iter base; };
static inline struct page *sg_page_iter_page(struct sg_page_iter *it){ return sg_page(it->sg); }
#define for_each_sgtable_page(sgt, piter, pgoffset) for((piter)->sg=(sgt)->sgl,(piter)->sg_pgoffset=(pgoffset); (piter)->sg; (piter)->sg=sg_next((piter)->sg))
#define for_each_sg_page(sgl, piter, nents, pgoffset) for((piter)->sg=(sgl),(piter)->sg_pgoffset=(pgoffset); (piter)->sg; (piter)->sg=sg_next((piter)->sg))
#endif

#ifndef _LKPI_SG_ALLOC_SEG
#define _LKPI_SG_ALLOC_SEG
int sg_alloc_table_from_pages_segment(struct sg_table *sgt, struct page **pages, unsigned n, unsigned off, unsigned long size, unsigned max_seg, unsigned gfp);
#endif

#ifndef _LKPI_SG_DMA_PAGE
#define _LKPI_SG_DMA_PAGE
#define for_each_sgtable_dma_page(sgt, dpiter, pgoffset) for((dpiter)->base.sg=(sgt)->sgl,(dpiter)->base.sg_pgoffset=(pgoffset); (dpiter)->base.sg; (dpiter)->base.sg=sg_next((dpiter)->base.sg))
#define sg_page_iter_dma_address(piter) sg_dma_address((piter)->base.sg)
#endif
