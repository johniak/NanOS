#ifndef _LKPI_HIGHMEM_H
#define _LKPI_HIGHMEM_H
#include <linux/mm.h>
static inline void *kmap_atomic(struct page *p){ return page_address(p); }
static inline void kunmap_atomic(void *a){ (void)a; }
static inline void *kmap_local_page(struct page *p){ return page_address(p); }
/* prot variant: the linear map is already cacheable-coherent here, so the pgprot is advisory. */
static inline void *kmap_local_page_prot(struct page *p, pgprot_t prot){ (void)prot; return page_address(p); }
#ifndef _LKPI_KMAP_LOCAL_FOLIO
#define _LKPI_KMAP_LOCAL_FOLIO
static inline void *kmap_local_folio(struct folio *f, size_t offset){ return (char *)page_address((struct page *)f) + offset; }
#endif
#ifndef _LKPI_KUNMAP_LOCAL
#define _LKPI_KUNMAP_LOCAL
static inline void kunmap_local(void *a){ (void)a; }
#endif
#ifndef _LKPI_KMAP
#define _LKPI_KMAP
static inline void *kmap(struct page *p){ return page_address(p); }
static inline void kunmap(struct page *p){ (void)p; }
#endif
static inline void memcpy_to_page(struct page *p, size_t off, const void *s, size_t n){ memcpy((char*)page_address(p)+off,s,n); }
static inline void memcpy_from_page(void *d, struct page *p, size_t off, size_t n){ memcpy(d,(char*)page_address(p)+off,n); }
/* folio byte helpers. Defined here (highmem.h is force-included via compat.h) so they are reachable
 * everywhere without an explicit <linux/pagemap.h> include (i915_gem_shmem reaches them via
 * shmem_fs.h in mainline). Shared sentinel with pagemap.h so only one copy is defined. */
#ifndef _LKPI_FOLIO_XFER
#define _LKPI_FOLIO_XFER
struct folio;
static inline size_t offset_in_folio(struct folio *f, unsigned long pos){ (void)f; return pos & (PAGE_SIZE - 1); }
static inline void memcpy_to_folio(struct folio *f, size_t off, const void *src, size_t n){ memcpy((char *)page_address((struct page *)f)+off, src, n); }
static inline void memcpy_from_folio(void *dst, struct folio *f, size_t off, size_t n){ memcpy(dst, (char *)page_address((struct page *)f)+off, n); }
#endif
static inline void copy_highpage(struct page *to, struct page *from){ memcpy(page_address(to), page_address(from), PAGE_SIZE); }
static inline void clear_highpage(struct page *p){ memset(page_address(p), 0, PAGE_SIZE); }
#endif
