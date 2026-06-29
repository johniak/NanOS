#ifndef _LKPI_PAGEMAP_H
#define _LKPI_PAGEMAP_H
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/gfp.h>
static inline void *folio_address(struct folio *f){ return (void*)f; }
static inline struct page *folio_page(struct folio *f, unsigned long n){ return (struct page*)((char*)f + n*PAGE_SIZE); }
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
#endif
