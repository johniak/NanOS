#ifndef _LKPI_SHMEM_FS_H
#define _LKPI_SHMEM_FS_H
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>   /* i915_gem_shmem reaches offset_in_folio/memcpy_to_folio via shmem_fs.h, as in mainline */
#define VM_NORESERVE 0x00200000
struct file *shmem_file_setup(const char *name, loff_t size, unsigned long flags);
static inline struct file *shmem_file_setup_with_mnt(struct vfsmount *mnt, const char *name, loff_t size, unsigned long flags){ (void)mnt; return shmem_file_setup(name, size, flags); }
struct folio *shmem_read_folio_gfp(struct address_space *mapping, unsigned long index, unsigned gfp);
static inline void shmem_truncate_range(struct inode *i, loff_t a, loff_t b){ (void)i;(void)a;(void)b; }
static inline int shmem_get_folio(struct inode *i, unsigned long idx, loff_t w, struct folio **fp, int sgp){ (void)i;(void)idx;(void)w;(void)sgp; *fp=0; return -1; }
/* shmem_read_mapping_page(_gfp): fetch the page backing `index` in a shmem address_space. Backed by
 * the shim's lazily-populated per-index page array (see kpi_misc.c shmem_*). */
struct page *shmem_read_mapping_page_gfp(struct address_space *mapping, unsigned long index, unsigned gfp);
static inline struct page *shmem_read_mapping_page(struct address_space *mapping, unsigned long index){ return shmem_read_mapping_page_gfp(mapping, index, 0); }
#endif
