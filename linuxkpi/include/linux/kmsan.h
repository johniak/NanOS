/*
 * linuxkpi/include/linux/kmsan.h — KMSAN (kernel memory sanitizer) hooks: all no-ops.
 */
#ifndef _LINUXKPI_LINUX_KMSAN_H
#define _LINUXKPI_LINUX_KMSAN_H

#include <linux/types.h>

static inline void kmsan_handle_dma(void *page, size_t offset, size_t size, int dir) { (void)page;(void)offset;(void)size;(void)dir; }
static inline void kmsan_handle_dma_sg(void *sg, int nents, int dir) { (void)sg;(void)nents;(void)dir; }

#endif /* _LINUXKPI_LINUX_KMSAN_H */
