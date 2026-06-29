#ifndef _LKPI_DMA_BUF_H
#define _LKPI_DMA_BUF_H
#include <linux/types.h>
#include <linux/scatterlist.h>
#include <linux/dma-direction.h>
#include <linux/iosys-map.h>
struct dma_buf; struct dma_buf_attachment; struct device; struct dma_resv;
struct dma_buf_ops {
  bool cache_sgt_mapping;
  int (*attach)(struct dma_buf*, struct dma_buf_attachment*);
  void (*detach)(struct dma_buf*, struct dma_buf_attachment*);
  struct sg_table *(*map_dma_buf)(struct dma_buf_attachment*, enum dma_data_direction);
  void (*unmap_dma_buf)(struct dma_buf_attachment*, struct sg_table*, enum dma_data_direction);
  void (*release)(struct dma_buf*);
  int (*mmap)(struct dma_buf*, struct vm_area_struct*);
  int (*vmap)(struct dma_buf*, struct iosys_map*);
  void (*vunmap)(struct dma_buf*, struct iosys_map*);
};
struct dma_buf {
  size_t size; const struct dma_buf_ops *ops; void *priv;
  struct dma_resv *resv; struct file *file; const char *exp_name;
};
struct dma_buf_attachment { struct dma_buf *dmabuf; struct device *dev; void *priv; void *importer_priv; };
struct dma_buf_export_info {
  const char *exp_name; void *owner; const struct dma_buf_ops *ops;
  size_t size; int flags; struct dma_resv *resv; void *priv;
};
#define DEFINE_DMA_BUF_EXPORT_INFO(name) struct dma_buf_export_info name = { .exp_name = KBUILD_MODNAME, .owner = 0 }
#ifdef __cplusplus
extern "C" {
#endif
struct dma_buf *dma_buf_export(struct dma_buf_export_info*);
int  dma_buf_fd(struct dma_buf*, int flags);
struct dma_buf *dma_buf_get(int fd);
void dma_buf_put(struct dma_buf*);
struct dma_buf_attachment *dma_buf_attach(struct dma_buf*, struct device*);
void dma_buf_detach(struct dma_buf*, struct dma_buf_attachment*);
struct sg_table *dma_buf_map_attachment(struct dma_buf_attachment*, enum dma_data_direction);
void dma_buf_unmap_attachment(struct dma_buf_attachment*, struct sg_table*, enum dma_data_direction);
#ifdef __cplusplus
}
#endif
#endif
