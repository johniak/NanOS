#ifndef _LKPI_VIRTIO_DMA_BUF_H
#define _LKPI_VIRTIO_DMA_BUF_H
#include <linux/dma-buf.h>
struct virtio_dma_buf_ops { int n; };
static inline struct dma_buf *virtio_dma_buf_export(void *exp){ (void)exp; return 0; }
static inline bool is_virtio_dma_buf(struct dma_buf *b){ (void)b; return false; }
static inline int virtio_dma_buf_get_uuid(struct dma_buf *b, void *u){ (void)b;(void)u; return -1; }
#endif
