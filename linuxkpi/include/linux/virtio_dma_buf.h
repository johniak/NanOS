#ifndef _LKPI_VIRTIO_DMA_BUF_H
#define _LKPI_VIRTIO_DMA_BUF_H
#include <linux/dma-buf.h>
#include <linux/uuid.h>
struct virtio_dma_buf_ops {
  struct dma_buf_ops ops;
  int (*device_attach)(struct dma_buf*, struct dma_buf_attachment*);
  int (*get_uuid)(struct dma_buf*, uuid_t*);
};
struct virtio_dma_buf_attach_info { int n; };
static inline int virtio_dma_buf_attach(struct dma_buf*d, struct dma_buf_attachment*a){ (void)d;(void)a; return 0; }
struct dma_buf *virtio_dma_buf_export(struct dma_buf_export_info*);
static inline bool is_virtio_dma_buf(struct dma_buf *b){ (void)b; return false; }
static inline int virtio_dma_buf_get_uuid(struct dma_buf *b, uuid_t *u){ (void)b;(void)u; return -1; }
#endif
