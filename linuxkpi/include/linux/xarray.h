#ifndef _LKPI_XARRAY_H
#define _LKPI_XARRAY_H
#include <linux/types.h>
#include <linux/idr.h>
#include <linux/spinlock.h>
struct xarray { struct idr idr; spinlock_t lock; };
#define DEFINE_XARRAY(name) struct xarray name
#define XA_FLAGS_ALLOC 1
#define XA_FLAGS_ALLOC1 2
static inline void xa_init_flags(struct xarray *xa, unsigned f){ (void)f; idr_init(&xa->idr); spin_lock_init(&xa->lock); }
static inline void xa_init(struct xarray *xa){ xa_init_flags(xa,0); }
static inline void xa_destroy(struct xarray *xa){ idr_destroy(&xa->idr); }
static inline void *xa_load(struct xarray *xa, unsigned long i){ return idr_find(&xa->idr,(int)i); }
static inline void *xa_store(struct xarray *xa, unsigned long i, void *p, unsigned gfp){ (void)gfp; idr_replace(&xa->idr,p,(int)i); return 0; }
static inline void *xa_erase(struct xarray *xa, unsigned long i){ return idr_remove(&xa->idr,(int)i); }
static inline bool xa_is_err(const void *e){ return false; }
static inline bool xa_empty(struct xarray *xa){ return idr_is_empty(&xa->idr); }
static inline void xa_lock(struct xarray *xa){ spin_lock(&xa->lock); }
static inline void xa_unlock(struct xarray *xa){ spin_unlock(&xa->lock); }
static inline int xa_alloc(struct xarray *xa, u32 *id, void *p, unsigned limit, unsigned gfp){ (void)limit;(void)gfp; int r=idr_alloc(&xa->idr,p,0,0,0); if(r<0)return r; *id=(u32)r; return 0; }
#define xa_for_each(xa, index, entry) for(index=0; ((entry)=xa_load((xa),index))!=0 || (index) < (unsigned long)(xa)->idr.cap; index++) if((entry))
#define xa_lock_irqsave(xa,f) do{ (f)=0; xa_lock(xa); }while(0)
#define xa_unlock_irqrestore(xa,f) do{ (void)(f); xa_unlock(xa); }while(0)
#define xa_limit_32b ((unsigned)0xffffffff)
#endif

#ifndef _LKPI_XARRAY_ALLOC
#define _LKPI_XARRAY_ALLOC
#define DEFINE_XARRAY_ALLOC(name) struct xarray name
#define DEFINE_XARRAY_ALLOC1(name) struct xarray name
#endif
