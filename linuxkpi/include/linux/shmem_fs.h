#ifndef _LKPI_SHMEM_FS_H
#define _LKPI_SHMEM_FS_H
#include <linux/fs.h>
#include <linux/mm.h>
#define VM_NORESERVE 0x00200000
struct file *shmem_file_setup(const char *name, loff_t size, unsigned long flags);
struct folio *shmem_read_folio_gfp(struct address_space *mapping, unsigned long index, unsigned gfp);
static inline void shmem_truncate_range(struct inode *i, loff_t a, loff_t b){ (void)i;(void)a;(void)b; }
static inline int shmem_get_folio(struct inode *i, unsigned long idx, loff_t w, struct folio **fp, int sgp){ (void)i;(void)idx;(void)w;(void)sgp; *fp=0; return -1; }
#endif
