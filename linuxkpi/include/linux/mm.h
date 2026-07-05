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

/*
 * struct page is an opaque token whose POINTER VALUE == the page's kernel virtual
 * address (see the whole shim's page convention). We give it size 1 (not just a
 * forward decl) so that pointer arithmetic on `struct page *` is BYTE arithmetic,
 * which is exactly what the address-token convention requires. Every arithmetic site
 * in the shim already casts to char-ptr or unsigned long first (nth_page, dma-mapping), so
 * they are unaffected; a size-1 type only matters for vendored code that does raw
 * `struct page * + n` expecting byte offsets, e.g. the i915 phys GEM backend's
 * `sg_page(sgl) + args->offset` (it stashes a raw vaddr via sg_assign_page and treats
 * the "page" as a byte address). Real Linux's mem_map page-stride arithmetic never
 * applies here (our pages are not a contiguous array), so no correct code regresses.
 * No member is ever accessed; the field exists only to complete the type.
 */
struct page { unsigned char __lkpi_addr_token; };

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
static inline struct page *alloc_pages(gfp_t gfp, unsigned int order) { return (struct page *)alloc_pages_exact(PAGE_SIZE << order, gfp); }
/* NUMA-node-targeted allocation (TTM page pool). Single-node/UMA here, so node is ignored. */
static inline struct page *alloc_pages_node(int nid, gfp_t gfp, unsigned int order) { (void)nid; return alloc_pages(gfp, order); }
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

#ifndef _LKPI_MM_COW
#define _LKPI_MM_COW
static inline int is_cow_mapping(unsigned long flags){ (void)flags; return 0; }
#endif

#ifndef _LKPI_MM_PAGE_X
#define _LKPI_MM_PAGE_X
/* page dirty/flags: the shim's page cache has no writeback, so "dirty" is a bookkeeping no-op that
 * reports the page was newly dirtied. NanOS RAM is never high memory (single flat map). */
static inline int set_page_dirty(struct page *p){ (void)p; return 1; }
static inline int PageHighMem(const struct page *p){ (void)p; return 0; }
static inline int PageReserved(const struct page *p){ (void)p; return 0; }
/* PFN rounding of a byte count/address. */
/* x86 PTE attribute bits (i915 GTT PPAT composition in intel_gtt.h uses PWT/PCD for uncached). */
#ifndef _PAGE_PWT
#define _PAGE_BIT_PWT 3
#define _PAGE_BIT_PCD 4
#define _PAGE_PWT  (1UL << _PAGE_BIT_PWT)
#define _PAGE_PCD  (1UL << _PAGE_BIT_PCD)
#define _PAGE_PAT  (1UL << 7)
#define _PAGE_PRESENT (1UL << 0)
#define _PAGE_RW      (1UL << 1)
#define _PAGE_CACHE_MASK  (_PAGE_PWT | _PAGE_PCD)
#endif
#ifndef PFN_UP
#define PFN_UP(x)   (((x) + PAGE_SIZE - 1) >> PAGE_SHIFT)
#define PFN_DOWN(x) ((x) >> PAGE_SHIFT)
#define PFN_PHYS(x) ((phys_addr_t)(x) << PAGE_SHIFT)
#define PHYS_PFN(x) ((unsigned long)((x) >> PAGE_SHIFT))
#endif
/* free order-N pages: the shim frees single pages; callers pass order 0 here. */
static inline void __free_pages(struct page *p, unsigned int order){ (void)order; __free_page(p); }
/* swap accounting: no swap on NanOS, so nothing is reclaimable this way. */
static inline long get_nr_swap_pages(void){ return 0; }
/* pagefault_disable/enable bracket a no-fault region; the shim's flat map never faults, so no-op. */
static inline void pagefault_disable(void){ }
static inline void pagefault_enable(void){ }
/* vm_mmap/call_mmap: the in-kernel KMS path never mmaps a shmem file into a user VMA in the shim,
 * so these report "not mapped" (0/-ENODEV). shmem GEM objects are accessed via kmap, not mmap. */
struct file; struct vm_area_struct;
static inline unsigned long vm_mmap(struct file *f, unsigned long addr, unsigned long len, unsigned long prot, unsigned long flag, unsigned long off){ (void)f;(void)addr;(void)len;(void)prot;(void)flag;(void)off; return 0; }
static inline int call_mmap(struct file *f, struct vm_area_struct *vma){ (void)f;(void)vma; return -19; }
/* page refcount: single flat allocator with no per-page refcount — get/put are inert. */
static inline void get_page(struct page *p){ (void)p; }
static inline void put_page(struct page *p){ (void)p; }
/* VMA protection/flags bits (subset i915 references). */
#ifndef VM_READ
#define VM_READ    0x00000001
#define VM_WRITE   0x00000002
#define VM_EXEC    0x00000004
#define VM_SHARED  0x00000008
#define VM_MAYWRITE 0x00000020
#define VM_IO      0x00004000
#define VM_PFNMAP  0x00000400
#define VM_DONTEXPAND 0x00040000
#define VM_DONTDUMP   0x04000000
#define VM_MIXEDMAP   0x10000000
#endif
static inline void vma_set_file(struct vm_area_struct *vma, struct file *file){ (void)vma;(void)file; }
/* Minimal x86 PTE primitives for i915_mm.c (remap_io_mapping). The shim's apply_to_page_range is a
 * stub that does not walk page tables (it never invokes the callback), so the pte helpers below are
 * only needed to satisfy the callback bodies' types; GEM userspace-mmap fault-in is a documented
 * follow-on (KMS-first bring-up drives the display without a user GEM mmap fault path). */
typedef struct { unsigned long pte; } pte_t;
struct mm_struct;
static inline pte_t pfn_pte(unsigned long pfn, pgprot_t prot){ pte_t p; p.pte = (pfn << PAGE_SHIFT) | (unsigned long)prot; return p; }
static inline pte_t pte_mkspecial(pte_t pte){ return pte; }
static inline unsigned long pte_pfn(pte_t pte){ return pte.pte >> PAGE_SHIFT; }
static inline void set_pte_at(struct mm_struct *mm, unsigned long addr, pte_t *ptep, pte_t pte){ (void)mm;(void)addr; if(ptep) *ptep = pte; }
typedef int (*pte_fn_t)(pte_t *pte, unsigned long addr, void *data);
static inline int apply_to_page_range(struct mm_struct *mm, unsigned long addr, unsigned long size, pte_fn_t fn, void *data){ (void)mm;(void)addr;(void)size;(void)fn;(void)data; return 0; }
static inline int zap_vma_ptes(struct vm_area_struct *vma, unsigned long address, unsigned long size){ (void)vma;(void)address;(void)size; return 0; }
static inline void flush_cache_range(struct vm_area_struct *vma, unsigned long start, unsigned long end){ (void)vma;(void)start;(void)end; }
/* fs_reclaim lockdep annotations: bracket a "may enter reclaim" region. No lockdep here → no-ops. */
static inline void fs_reclaim_acquire(gfp_t gfp){ (void)gfp; }
static inline void fs_reclaim_release(gfp_t gfp){ (void)gfp; }
/* VMA_ITERATOR/for_each_vma: i915 userptr walks a mm's VMAs. NanOS drives KMS in-kernel with no user
 * VMA graph to walk, so the iterator starts empty (loops execute zero times). */
struct vma_iterator { int _unused; };
#define VMA_ITERATOR(name, mm, addr) struct vma_iterator name = { 0 }
#define for_each_vma(vmi, vma) for ((vma) = 0; (vma); )
#define for_each_vma_range(vmi, vma, end) for ((vma) = 0; (vma); )
/* single flat page map: nth_page is pointer arithmetic; kmap maps to the page's linear address. */
static inline struct page *nth_page(struct page *p, unsigned long n){ return (struct page *)((char *)p + n * PAGE_SIZE); }
#ifndef _LKPI_KMAP
#define _LKPI_KMAP
static inline void *kmap(struct page *p){ return page_address(p); }
static inline void kunmap(struct page *p){ (void)p; }
#endif
/* mm read/write lock: KMS runs in-kernel with no user mm to lock — no-ops. */
struct mm_struct;
static inline void mmap_read_lock(struct mm_struct *mm){ (void)mm; }
static inline void mmap_read_unlock(struct mm_struct *mm){ (void)mm; }
static inline void mmap_write_lock(struct mm_struct *mm){ (void)mm; }
static inline void mmap_write_unlock(struct mm_struct *mm){ (void)mm; }
static inline int mmap_write_lock_killable(struct mm_struct *mm){ (void)mm; return 0; }
static inline int mmap_read_lock_killable(struct mm_struct *mm){ (void)mm; return 0; }
/* no user VMA graph in the shim → find_vma finds nothing. */
static inline struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr){ (void)mm;(void)addr; return 0; }
static inline struct vm_area_struct *vma_lookup(struct mm_struct *mm, unsigned long addr){ (void)mm;(void)addr; return 0; }
/* total RAM in pages: report a fixed large value (i915 sizes caches against it). */
static inline unsigned long totalram_pages(void){ return 512UL * 1024 * 1024 / PAGE_SIZE; }
/* struct sysinfo / si_meminfo: memory totals for TTM's page-pool sizing. Fields match Linux
 * semantics — totalram/freeram are in PAGE-sized `mem_unit`s. We report totalram_pages() and a
 * conservative ~half free; TTM only uses these to cap its pool, so exact freeram is not critical. */
struct sysinfo {
	long uptime; unsigned long loads[3];
	unsigned long totalram, freeram, sharedram, bufferram;
	unsigned long totalswap, freeswap;
	unsigned short procs, pad;
	unsigned long totalhigh, freehigh;
	unsigned int mem_unit;
};
static inline void si_meminfo(struct sysinfo *si){
	si->totalram = totalram_pages();
	si->freeram  = si->totalram / 2;
	si->sharedram = si->bufferram = 0;
	si->totalhigh = si->freehigh = 0;
	si->totalswap = si->freeswap = 0;
	si->mem_unit = PAGE_SIZE;
}
/* the shim never runs in kswapd/reclaim context. */
static inline int current_is_kswapd(void){ return 0; }
static inline int page_mapped(struct page *p){ (void)p; return 0; }
static inline int page_count(struct page *p){ (void)p; return 1; }
/* page-flag setters/clearers i915 swap-out touches (no writeback pipeline → bookkeeping no-ops). */
static inline void SetPageReclaim(struct page *p){ (void)p; }
static inline void ClearPageReclaim(struct page *p){ (void)p; }
static inline void set_page_writeback(struct page *p){ (void)p; }
static inline void end_page_writeback(struct page *p){ (void)p; }
static inline int PageWriteback(struct page *p){ (void)p; return 0; }
static inline int PageDirty(struct page *p){ (void)p; return 0; }
static inline int PageLocked(struct page *p){ (void)p; return 0; }
#ifndef _LKPI_MARK_PAGE_ACCESSED
#define _LKPI_MARK_PAGE_ACCESSED
static inline void mark_page_accessed(struct page *p){ (void)p; }
#endif
#ifndef VM_FAULT_RETRY
#define VM_FAULT_OOM            0x000001
#define VM_FAULT_SIGBUS         0x000002
#define VM_FAULT_MAJOR          0x000004
#define VM_FAULT_HWPOISON       0x000010
#define VM_FAULT_HWPOISON_LARGE 0x000020
#define VM_FAULT_SIGSEGV        0x000040
#define VM_FAULT_NOPAGE         0x000100
#define VM_FAULT_LOCKED         0x000200
#define VM_FAULT_RETRY          0x000400
#define VM_FAULT_FALLBACK       0x000800
#define VM_FAULT_DONE_COW       0x001000
#define VM_FAULT_NEEDDSYNC      0x002000
/* the set of fault results that mean "failed" (TTM's fault handler checks VM_FAULT_ERROR). */
#define VM_FAULT_ERROR (VM_FAULT_OOM | VM_FAULT_SIGBUS | VM_FAULT_SIGSEGV | \
			VM_FAULT_HWPOISON | VM_FAULT_HWPOISON_LARGE | VM_FAULT_FALLBACK)
#endif
#ifndef PROT_READ
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#endif
#ifndef MAP_SHARED
#define MAP_SHARED   0x01
#define MAP_PRIVATE  0x02
#define MAP_FIXED    0x10
#endif
#ifndef FAULT_FLAG_RETRY_NOWAIT
#define FAULT_FLAG_WRITE        0x01
#define FAULT_FLAG_ALLOW_RETRY  0x04
#define FAULT_FLAG_RETRY_NOWAIT 0x08
#define FAULT_FLAG_KILLABLE     0x10
#endif
/* First attempt of a retryable fault? (ALLOW_RETRY set and TRIED not yet set.) TTM's fault
 * handler uses it to decide whether it may drop mmap_lock and retry. */
#ifndef FAULT_FLAG_TRIED
#define FAULT_FLAG_TRIED        0x20
#endif
static inline bool fault_flag_allow_retry_first(unsigned int flags){
	return (flags & FAULT_FLAG_ALLOW_RETRY) && !(flags & FAULT_FLAG_TRIED);
}
/* Should freed pages be poisoned/zeroed? NanOS has no init_on_free hardening, so no. */
static inline bool want_init_on_free(void){ return false; }
#endif
