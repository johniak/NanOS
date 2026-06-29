#ifndef _LKPI_DMA_BUF_H
#define _LKPI_DMA_BUF_H
#include <linux/types.h>
#include <linux/scatterlist.h>
#include <linux/dma-direction.h>
#include <linux/iosys-map.h>
#include <linux/mm_types.h>
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

#ifndef _LKPI_DMABUF_VMAP
#define _LKPI_DMABUF_VMAP
struct iosys_map;
static inline int dma_buf_vmap(struct dma_buf *b, struct iosys_map *m){ (void)b;(void)m; return -1; }
static inline void dma_buf_vunmap(struct dma_buf *b, struct iosys_map *m){ (void)b;(void)m; }
static inline int dma_buf_vmap_unlocked(struct dma_buf *b, struct iosys_map *m){ (void)b;(void)m; return -1; }
static inline void dma_buf_vunmap_unlocked(struct dma_buf *b, struct iosys_map *m){ (void)b;(void)m; }
#endif

#ifndef _LKPI_DMABUF_CPU
#define _LKPI_DMABUF_CPU
static inline int dma_buf_begin_cpu_access(struct dma_buf *b, enum dma_data_direction d){ (void)b;(void)d; return 0; }
static inline int dma_buf_end_cpu_access(struct dma_buf *b, enum dma_data_direction d){ (void)b;(void)d; return 0; }
#endif

#ifndef _LKPI_DMABUF_X2
#define _LKPI_DMABUF_X2
static inline int dma_buf_mmap(struct dma_buf *b, struct vm_area_struct *v, unsigned long off){ (void)b;(void)v;(void)off; return -1; }
static inline void get_dma_buf(struct dma_buf *b){ (void)b; }
#endif

#ifndef _LKPI_DMABUF_ATTACH_UNLOCKED
#define _LKPI_DMABUF_ATTACH_UNLOCKED
static inline struct sg_table *dma_buf_map_attachment_unlocked(struct dma_buf_attachment *a, enum dma_data_direction d){ (void)a;(void)d; return 0; }
static inline void dma_buf_unmap_attachment_unlocked(struct dma_buf_attachment *a, struct sg_table *s, enum dma_data_direction d){ (void)a;(void)s;(void)d; }
#endif
