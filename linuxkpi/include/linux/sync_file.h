#ifndef _LKPI_SYNC_FILE_H
#define _LKPI_SYNC_FILE_H
#include <linux/dma-fence.h>
struct sync_file; struct file;
static inline struct sync_file *sync_file_create(struct dma_fence *f){ (void)f; return 0; }
static inline struct dma_fence *sync_file_get_fence(int fd){ (void)fd; return 0; }
#endif
