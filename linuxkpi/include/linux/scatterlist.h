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

static inline int sg_is_chain(struct scatterlist *sg) { return !!(sg->page_link & SG_CHAIN); }
static inline int sg_is_last(struct scatterlist *sg)  { return !!(sg->page_link & SG_END); }

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

#endif /* _LINUXKPI_LINUX_SCATTERLIST_H */
