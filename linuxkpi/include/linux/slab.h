/*
 * linuxkpi/include/linux/slab.h — kmalloc family for the NanOS LinuxKPI shim, backed by
 * the kernel heap (knx_malloc/knx_free). Every block carries a small size header so
 * krealloc/ksize work without a slab; payload is returned 16-byte aligned.
 */
#ifndef _LINUXKPI_LINUX_SLAB_H
#define _LINUXKPI_LINUX_SLAB_H

#include <linux/types.h>
#include <linux/gfp.h>
#include <linux/string.h>   /* the inline kmemdup/memdup_user helpers below use memcpy */
#include <linux/poison.h>   /* POISON_INUSE etc.; Linux's slab.h pulls poison.h, and i915 reaches the
                             * poison bytes only through slab.h (they don't include poison.h directly) */
#ifndef NANOS_HOST_TEST
#include <linux/mm.h>   /* slab pulls mm in mainline; gives page helpers to .c that only include slab.h */
#endif

/* Zero-size allocation sentinel (Linux slab): kmalloc(0) returns ZERO_SIZE_PTR, a non-NULL pointer
 * that faults on deref but is safe to kfree. i915 compares alloc results against it. */
#ifndef ZERO_SIZE_PTR
#define ZERO_SIZE_PTR ((void *)16)
#define ZERO_OR_NULL_PTR(x) ((unsigned long)(x) <= (unsigned long)ZERO_SIZE_PTR)
#endif

#ifdef __cplusplus
extern "C" {
#endif

void *kmalloc(size_t size, gfp_t flags);
void *kzalloc(size_t size, gfp_t flags);
void *kcalloc(size_t n, size_t size, gfp_t flags);
void *kmalloc_array(size_t n, size_t size, gfp_t flags);
void *krealloc(const void *p, size_t new_size, gfp_t flags);
void  kfree(const void *p);
size_t ksize(const void *p);

/* Linux aliases that map onto the same heap for the shim. */
void *kvmalloc(size_t size, gfp_t flags);
void *kvzalloc(size_t size, gfp_t flags);
void  kvfree(const void *p);
void *vmalloc(size_t size);
void *vzalloc(size_t size);
void  vfree(const void *p);

char *kstrdup(const char *s, gfp_t flags);

#ifdef __cplusplus
}
#endif

#endif /* _LINUXKPI_LINUX_SLAB_H */

#ifndef _LKPI_SLAB_EXTRA
#define _LKPI_SLAB_EXTRA
static inline void *kvmalloc_array(size_t n, size_t s, gfp_t f){ return kmalloc(n*s, f); }
static inline void *kvcalloc(size_t n, size_t s, gfp_t f){ return kzalloc(n*s, f); }
struct kmem_cache { size_t size; };
static inline struct kmem_cache *kmem_cache_create(const char *n, unsigned sz, unsigned al, unsigned long fl, void *ctor){ (void)n;(void)al;(void)fl;(void)ctor; struct kmem_cache *c=(struct kmem_cache*)kmalloc(sizeof(*c),0); if(c)c->size=sz; return c; }
static inline void kmem_cache_destroy(struct kmem_cache *c){ kfree(c); }
static inline void *kmem_cache_alloc(struct kmem_cache *c, gfp_t f){ return kmalloc(c->size, f); }
static inline void *kmem_cache_zalloc(struct kmem_cache *c, gfp_t f){ return kzalloc(c->size, f); }
static inline void kmem_cache_free(struct kmem_cache *c, void *p){ (void)c; kfree(p); }
static inline void *memdup_user(const void *src, size_t len){ void *p=kmalloc(len,0); if(p)memcpy(p,src,len); return p; }
static inline void *vmemdup_user(const void *src, size_t len){ return memdup_user(src,len); }
#endif

#ifndef _LKPI_SLAB_ALIGN
#define _LKPI_SLAB_ALIGN
#define ARCH_KMALLOC_MINALIGN 16
#define ARCH_DMA_MINALIGN 16
#endif

#ifndef _LKPI_SLAB_X2
#define _LKPI_SLAB_X2
static inline void *krealloc_array(void *p, size_t n, size_t s, gfp_t f){ return krealloc(p, n*s, f); }
static inline void kfree_const(const void *p){ kfree(p); }
static inline void *kmalloc_node_track_caller(size_t n, gfp_t f, int node){ (void)node; return kmalloc(n,f); }
static inline char *kstrdup_const(const char *s, gfp_t f){ return kstrdup(s,f); }
static inline void *kmemdup(const void *src, size_t len, gfp_t f){ void *p=kmalloc(len,f); if(p)memcpy(p,src,len); return p; }
static inline void *kvmemdup(const void *src, size_t len, gfp_t f){ return kmemdup(src,len,f); }
#endif
