/*
 * linuxkpi/include/linux/mm.h — page/memory helpers for the shim.
 *
 * Page model: NanOS's kernel space is identity-mapped, so a `struct page *` is treated as
 * a token whose VALUE is the page's kernel virtual address (== physical address). struct
 * page is never dereferenced by the vendored code we compile; page_address()/virt_to_page()
 * just cast. alloc_pages_exact() returns page-aligned contiguous memory (kpi_mm.c).
 */
#ifndef _LINUXKPI_LINUX_MM_H
#define _LINUXKPI_LINUX_MM_H

#include <linux/types.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/mm_types.h>

#define PAGE_SHIFT 12
#define PAGE_SIZE  (1UL << PAGE_SHIFT)
#define PAGE_MASK  (~(PAGE_SIZE - 1))
#define PAGE_ALIGN(addr)      ALIGN((unsigned long)(addr), PAGE_SIZE)
#define PAGE_ALIGNED(addr)    IS_ALIGNED((unsigned long)(addr), PAGE_SIZE)
#define offset_in_page(p)     ((unsigned long)(p) & ~PAGE_MASK)

struct page;  /* opaque token: its pointer value == the page's kernel virtual address */

static inline unsigned int get_order(unsigned long size) {
	unsigned int order = 0;
	size = (size - 1) >> PAGE_SHIFT;
	while (size) { order++; size >>= 1; }
	return order;
}

static inline void *page_address(const struct page *p) { return (void *)p; }
static inline struct page *virt_to_page(const void *addr) {
	return (struct page *)((unsigned long)addr & PAGE_MASK);
}
static inline unsigned long page_to_pfn(const struct page *p) { return (unsigned long)p >> PAGE_SHIFT; }
static inline struct page *pfn_to_page(unsigned long pfn) { return (struct page *)(pfn << PAGE_SHIFT); }
static inline phys_addr_t page_to_phys(const struct page *p) { return (phys_addr_t)(unsigned long)p; }
static inline void *page_to_virt(const struct page *p) { return (void *)p; }
static inline phys_addr_t virt_to_phys(const volatile void *addr) { return (phys_addr_t)(unsigned long)addr; }
static inline void *phys_to_virt(phys_addr_t pa) { return (void *)(unsigned long)pa; }

#ifdef __cplusplus
extern "C" {
#endif
void *alloc_pages_exact(size_t size, gfp_t gfp);
void  free_pages_exact(void *virt, size_t size);
#ifdef __cplusplus
}
#endif

/* alloc_page/__get_free_page return page-aligned memory via the same allocator. */
static inline struct page *alloc_page(gfp_t gfp) { return (struct page *)alloc_pages_exact(PAGE_SIZE, gfp); }
static inline void __free_page(struct page *p) { free_pages_exact((void *)p, PAGE_SIZE); }
static inline unsigned long __get_free_page(gfp_t gfp) { return (unsigned long)alloc_pages_exact(PAGE_SIZE, gfp); }
static inline unsigned long __get_free_pages(gfp_t gfp, unsigned int order) { return (unsigned long)alloc_pages_exact(PAGE_SIZE << order, gfp); }
static inline void free_page(unsigned long addr) { free_pages_exact((void *)addr, PAGE_SIZE); }
static inline void free_pages(unsigned long addr, unsigned int order) { free_pages_exact((void *)addr, PAGE_SIZE << order); }

static inline void unmap_mapping_range(void *m, unsigned long h, unsigned long e, int z){(void)m;(void)h;(void)e;(void)z;}
/* page protections + user-mapping helpers (inert: bridge drives KMS in-kernel, no DRM mmap) */
static inline pgprot_t pgprot_writecombine(pgprot_t p){ return p; }
static inline pgprot_t pgprot_noncached(pgprot_t p){ return p; }
static inline pgprot_t pgprot_decrypted(pgprot_t p){ return p; }
static inline pgprot_t vm_get_page_prot(unsigned long f){ (void)f; return 0; }
static inline void vm_flags_set(struct vm_area_struct *v, unsigned long f){ v->vm_flags |= f; }
static inline void vm_flags_clear(struct vm_area_struct *v, unsigned long f){ v->vm_flags &= ~f; }
static inline int remap_pfn_range(struct vm_area_struct *v, unsigned long a, unsigned long pfn, unsigned long s, pgprot_t p){ (void)v;(void)a;(void)pfn;(void)s;(void)p; return 0; }
static inline int io_remap_pfn_range(struct vm_area_struct *v, unsigned long a, unsigned long pfn, unsigned long s, pgprot_t p){ (void)v;(void)a;(void)pfn;(void)s;(void)p; return 0; }
#endif /* _LINUXKPI_LINUX_MM_H */

#ifndef _LKPI_MM_EXTRA
#define _LKPI_MM_EXTRA
static inline struct page *vmalloc_to_page(const void *addr){ return virt_to_page(addr); }
static inline int is_vmalloc_addr(const void *x){ (void)x; return 0; }
#endif

#ifndef _LKPI_MM_VMA
#define _LKPI_MM_VMA
static inline unsigned long vma_pages(struct vm_area_struct *v){ return (v->vm_end-v->vm_start)>>PAGE_SHIFT; }
#endif

#ifndef _LKPI_MM_VMF
#define _LKPI_MM_VMF
typedef unsigned long vm_fault_t_lkpi_;
static inline unsigned long vmf_insert_pfn(struct vm_area_struct *v, unsigned long a, unsigned long pfn){ (void)v;(void)a;(void)pfn; return 0x100; }
static inline unsigned long vmf_insert_mixed(struct vm_area_struct *v, unsigned long a, unsigned long pfn){ (void)v;(void)a;(void)pfn; return 0x100; }
static inline unsigned long vmf_insert_pfn_prot(struct vm_area_struct *v, unsigned long a, unsigned long pfn, pgprot_t p){ (void)v;(void)a;(void)pfn;(void)p; return 0x100; }
#endif

#ifndef _LKPI_MM_MAPPING
#define _LKPI_MM_MAPPING
static inline void mapping_set_gfp_mask(struct address_space *m, unsigned g){ (void)m;(void)g; }
static inline unsigned long invalidate_mapping_pages(struct address_space *m, unsigned long s, unsigned long e){ (void)m;(void)s;(void)e; return 0; }
#endif
