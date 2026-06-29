#ifndef _LKPI_HIGHMEM_H
#define _LKPI_HIGHMEM_H
#include <linux/mm.h>
static inline void *kmap_atomic(struct page *p){ return page_address(p); }
static inline void kunmap_atomic(void *a){ (void)a; }
static inline void *kmap_local_page(struct page *p){ return page_address(p); }
static inline void kunmap_local(void *a){ (void)a; }
static inline void *kmap(struct page *p){ return page_address(p); }
static inline void kunmap(struct page *p){ (void)p; }
static inline void memcpy_to_page(struct page *p, size_t off, const void *s, size_t n){ memcpy((char*)page_address(p)+off,s,n); }
static inline void memcpy_from_page(void *d, struct page *p, size_t off, size_t n){ memcpy(d,(char*)page_address(p)+off,n); }
#endif
