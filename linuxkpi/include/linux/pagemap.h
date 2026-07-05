#ifndef _LKPI_PAGEMAP_H
#define _LKPI_PAGEMAP_H
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/gfp.h>
/* mem_map model: a folio* IS a page* (mem_map entry). folio_address maps it to the DATA address via
 * page_address (NOT (void*)f, which would be the metadata-array pointer); folio_page(f,n) is the
 * n-th frame = mem_map-adjacent entry f+n (single-page folios normally use n==0). */
static inline void *folio_address(struct folio *f){ return page_address((struct page*)f); }
static inline struct page *folio_page(struct folio *f, unsigned long n){ return (struct page*)f + n; }
static inline struct page *folio_file_page(struct folio *f, unsigned long i){ (void)i; return (struct page*)f; }
static inline unsigned long folio_nr_pages(struct folio *f){ (void)f; return 1; }
static inline unsigned long folio_pfn(struct folio *f){ return page_to_pfn((struct page*)f); }
static inline void folio_put(struct folio *f){ (void)f; }
static inline void folio_get(struct folio *f){ (void)f; }
static inline void folio_mark_accessed(struct folio *f){ (void)f; }
static inline void folio_mark_dirty(struct folio *f){ (void)f; }
static inline unsigned folio_size(struct folio *f){ (void)f; return PAGE_SIZE; }
static inline unsigned mapping_gfp_mask(struct address_space *m){ (void)m; return GFP_KERNEL; }
static inline unsigned mapping_gfp_constraint(struct address_space *m, unsigned g){ (void)m; return g; }
static inline void mapping_set_unevictable(struct address_space *m){ (void)m; }
static inline void mapping_clear_unevictable(struct address_space *m){ (void)m; }
static inline void mapping_set_large_folios(struct address_space *m){ (void)m; }
#define FGP_CREAT 0
struct folio_batch { unsigned nr; struct folio *folios[16]; };
static inline void folio_batch_init(struct folio_batch *b){ b->nr=0; }
static inline void __folio_batch_release(struct folio_batch *b){ b->nr=0; }
static inline void folio_batch_release(struct folio_batch *b){ b->nr=0; }
static inline int check_move_unevictable_folios(struct folio_batch *b){ (void)b; return 0; }
#endif



#ifndef _LKPI_PAGEMAP_FOLIO2
#define _LKPI_PAGEMAP_FOLIO2
static inline struct folio *page_folio(struct page *p){ return (struct folio*)p; }
static inline unsigned folio_batch_add(struct folio_batch *b, struct folio *f){ if(b->nr<16)b->folios[b->nr++]=f; return 16-b->nr; }
static inline unsigned folio_batch_count(struct folio_batch *b){ return b->nr; }
static inline unsigned folio_batch_space(struct folio_batch *b){ return 16-b->nr; }
/* page-cache lookups: i915 shmem swap-out path. Backed by the lazily-populated shmem page array;
 * the shim doesn't index by struct page here, so report "not present" (NULL) and callers re-fault. */
static inline struct page *find_lock_page(struct address_space *mapping, unsigned long index){ (void)mapping;(void)index; return 0; }
static inline struct page *find_get_page(struct address_space *mapping, unsigned long index){ (void)mapping;(void)index; return 0; }
static inline void unlock_page(struct page *p){ (void)p; }
static inline void lock_page(struct page *p){ (void)p; }
static inline int clear_page_dirty_for_io(struct page *p){ (void)p; return 0; }
#ifndef _LKPI_KMAP_LOCAL_FOLIO
#define _LKPI_KMAP_LOCAL_FOLIO
static inline void *kmap_local_folio(struct folio *f, size_t offset){ return (char *)page_address((struct page *)f) + offset; }
static inline size_t offset_in_folio(struct folio *f, unsigned long pos){ (void)f; return pos & (PAGE_SIZE - 1); }
static inline void memcpy_to_folio(struct folio *f, size_t off, const void *src, size_t n){ memcpy((char *)page_address((struct page *)f)+off, src, n); }
static inline void memcpy_from_folio(void *dst, struct folio *f, size_t off, size_t n){ memcpy(dst, (char *)page_address((struct page *)f)+off, n); }
static inline void *kmap_local_folio_offset(struct folio *f, size_t off){ return (char *)page_address((struct page *)f)+off; }
#ifndef _LKPI_KUNMAP_LOCAL
#define _LKPI_KUNMAP_LOCAL
static inline void kunmap_local(void *addr){ (void)addr; }
#endif
#endif
static inline int trylock_page(struct page *p){ (void)p; return 1; }
static inline void wait_on_page_writeback(struct page *p){ (void)p; }
#endif
