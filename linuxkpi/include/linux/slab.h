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

/* kmem_cache creation flags — the shim's allocator ignores them (no per-cache tuning), but they must
 * be defined so KMEM_CACHE(T, SLAB_HWCACHE_ALIGN | ...) and friends compile. */
#ifndef SLAB_HWCACHE_ALIGN
#define SLAB_HWCACHE_ALIGN   0x00002000u
#define SLAB_RECLAIM_ACCOUNT 0x00020000u
#define SLAB_TYPESAFE_BY_RCU 0x00080000u
#define SLAB_POISON          0x00000800u
#define SLAB_CACHE_DMA       0x00004000u
#define SLAB_PANIC           0x00040000u
#define SLAB_ACCOUNT         0x04000000u
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
/* A cache may carry a constructor. In real Linux the ctor runs ONCE when a slab
 * page is created and objects keep that constructed state across free/reuse
 * (SLAB_TYPESAFE_BY_RCU) — so callers like i915's __i915_request_create() do NOT
 * re-initialise ctor-set fields (e.g. rq->submit.fn / rq->semaphore.fn). This shim
 * has no object pooling: every kmem_cache_alloc() is a fresh kmalloc(). To give
 * each fresh object the same constructed state the ctor MUST run on every alloc,
 * exactly as it would on a first-time slab construction. Dropping the ctor (the
 * old behaviour) left those fields uninitialised — i915 request submit/semaphore
 * fence ->fn stayed as heap garbage, so the first engine-park kernel-context
 * request #GP'd on `call *fn` (non-canonical). */
struct kmem_cache { size_t size; void (*ctor)(void *); };
static inline struct kmem_cache *kmem_cache_create(const char *n, unsigned sz, unsigned al, unsigned long fl, void (*ctor)(void *)){ (void)n;(void)al;(void)fl; struct kmem_cache *c=(struct kmem_cache*)kmalloc(sizeof(*c),0); if(c){ c->size=sz; c->ctor=ctor; } return c; }
static inline void kmem_cache_destroy(struct kmem_cache *c){ kfree(c); }
static inline void *kmem_cache_alloc(struct kmem_cache *c, gfp_t f){ void *p=kmalloc(c->size, f); if(p&&c->ctor)c->ctor(p); return p; }
static inline void *kmem_cache_zalloc(struct kmem_cache *c, gfp_t f){ void *p=kzalloc(c->size, f); if(p&&c->ctor)c->ctor(p); return p; }
static inline void kmem_cache_free(struct kmem_cache *c, void *p){ (void)c; kfree(p); }
static inline void *memdup_user(const void *src, size_t len){ void *p=kmalloc(len,0); if(p)memcpy(p,src,len); return p; }
static inline void *vmemdup_user(const void *src, size_t len){ return memdup_user(src,len); }
/* like memdup_user but NUL-terminates (for user strings of known length). */
static inline void *memdup_user_nul(const void *src, size_t len){ char *p=(char*)kmalloc(len+1,0); if(p){ memcpy(p,src,len); p[len]=0; } return p; }
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
static inline void *kmemdup_array(const void *src, size_t n, size_t size, gfp_t f){ return kmemdup(src, n*size, f); }
#endif

#ifndef _LKPI_SLAB_KMEM_CACHE
#define _LKPI_SLAB_KMEM_CACHE
/* KMEM_CACHE(struct T, flags): named cache sized/aligned for that struct (canonical kernel macro). */
#define KMEM_CACHE(__struct, __flags) \
	kmem_cache_create(#__struct, sizeof(struct __struct), __alignof__(struct __struct), (__flags), NULL)
/* might_alloc() is a might_sleep()/lockdep annotation; no-op in the shim. */
#define might_alloc(gfp) do { (void)(gfp); } while (0)
#endif
