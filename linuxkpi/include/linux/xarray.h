#ifndef _LKPI_XARRAY_H
#define _LKPI_XARRAY_H
#include <linux/types.h>
#include <linux/idr.h>
#include <linux/spinlock.h>
struct xarray { struct idr idr; spinlock_t xa_lock; };
#define DEFINE_XARRAY(name) struct xarray name
#define XA_FLAGS_ALLOC 1
#define XA_FLAGS_ALLOC1 2
#define XA_FLAGS_LOCK_IRQ 4
#define XA_FLAGS_LOCK_BH  8
static inline void xa_init_flags(struct xarray *xa, unsigned f){ (void)f; idr_init(&xa->idr); spin_lock_init(&xa->xa_lock); }
static inline void xa_init(struct xarray *xa){ xa_init_flags(xa,0); }
static inline void xa_destroy(struct xarray *xa){ idr_destroy(&xa->idr); }
static inline void *xa_load(struct xarray *xa, unsigned long i){ return idr_find(&xa->idr,(int)i); }
static inline void *xa_store(struct xarray *xa, unsigned long i, void *p, unsigned gfp){ (void)gfp; idr_replace(&xa->idr,p,(int)i); return 0; }
/* __xa_store is the caller-locked variant; same effect in the shim (xa_lock is a plain spinlock). */
static inline void *__xa_store(struct xarray *xa, unsigned long i, void *p, unsigned gfp){ return xa_store(xa,i,p,gfp); }
static inline void *xa_erase(struct xarray *xa, unsigned long i){ return idr_remove(&xa->idr,(int)i); }
static inline void *__xa_erase(struct xarray *xa, unsigned long i){ return xa_erase(xa,i); }
static inline void *xa_erase_irq(struct xarray *xa, unsigned long i){ return xa_erase(xa,i); }
static inline void *xa_store_irq(struct xarray *xa, unsigned long i, void *p, unsigned gfp){ return xa_store(xa,i,p,gfp); }
/* value entries: XArray can store small integers tagged in the pointer (LSB set). */
static inline void *xa_mk_value(unsigned long v){ return (void *)((v << 1) | 1UL); }
static inline unsigned long xa_to_value(const void *e){ return (unsigned long)e >> 1; }
static inline bool xa_is_value(const void *e){ return (unsigned long)e & 1UL; }
static inline bool xa_is_err(const void *e){ return false; }
static inline bool xa_empty(struct xarray *xa){ return idr_is_empty(&xa->idr); }
static inline void xa_lock(struct xarray *xa){ spin_lock(&xa->xa_lock); }
static inline void xa_unlock(struct xarray *xa){ spin_unlock(&xa->xa_lock); }
struct xa_limit; static inline int xa_alloc(struct xarray *xa, u32 *id, void *p, struct xa_limit limit, unsigned gfp);
#define xa_for_each(xa, index, entry) for(index=0; ((entry)=xa_load((xa),index))!=0 || (index) < (unsigned long)(xa)->idr.cap; index++) if((entry))
#define xa_lock_irqsave(xa,f) do{ (f)=0; xa_lock(xa); }while(0)
#define xa_unlock_irqrestore(xa,f) do{ (void)(f); xa_unlock(xa); }while(0)
#define xa_lock_irq(xa)   xa_lock(xa)
#define xa_unlock_irq(xa) xa_unlock(xa)
#define xa_lock_bh(xa)    xa_lock(xa)
#define xa_unlock_bh(xa)  xa_unlock(xa)
#endif

#ifndef _LKPI_XARRAY_ALLOC
#define _LKPI_XARRAY_ALLOC
#define DEFINE_XARRAY_ALLOC(name) struct xarray name
#define DEFINE_XARRAY_ALLOC1(name) struct xarray name
#endif

#ifndef _LKPI_XA_ERR
#define _LKPI_XA_ERR
static inline int xa_err(void *e){ (void)e; return 0; }
#endif

#ifndef _LKPI_XA_LIMIT
#define _LKPI_XA_LIMIT
struct xa_limit { unsigned min, max; };
#define XA_LIMIT(_min,_max) (struct xa_limit){ .min=(_min), .max=(_max) }
static inline int xa_alloc(struct xarray *xa, u32 *id, void *p, struct xa_limit limit, unsigned gfp){ (void)limit;(void)gfp; int r=idr_alloc(&xa->idr,p,0,0,0); if(r<0)return r; *id=(u32)r; return 0; }
/* the id-range limits are struct xa_limit values (not bare integers) — xa_alloc takes them by value. */
#define xa_limit_32b  XA_LIMIT(0, 0xffffffffU)
#define xa_limit_31b  XA_LIMIT(0, 0x7fffffffU)
static inline int xa_alloc_cyclic_irq(struct xarray *xa, u32 *id, void *p, struct xa_limit limit, u32 *next, unsigned gfp){ (void)limit;(void)next;(void)gfp; int r=idr_alloc(&xa->idr,p,0,0,0); if(r<0)return r; *id=(u32)r; return 0; }
#endif
