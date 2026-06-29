/*
 * linuxkpi/include/linux/idr.h — id allocator (idr) + id bitmap (ida) for the LinuxKPI
 * shim. Backed by a growable id→pointer array (kpi_idr.c). Lowest-free-id allocation,
 * matching the subset of Linux semantics the DRM/virtio drivers rely on.
 */
#ifndef _LINUXKPI_LINUX_IDR_H
#define _LINUXKPI_LINUX_IDR_H

#include <linux/types.h>
#include <linux/gfp.h>

#ifdef __cplusplus
extern "C" {
#endif

struct idr {
	void   **slots;   /* id -> pointer (NULL = free) */
	int      cap;     /* allocated length of slots */
	int      base;    /* idr_init_base: lowest id handed out */
};

struct ida {
	struct idr idr;   /* an ida is an idr storing a non-NULL sentinel */
};

#define IDR_INIT(name)  { 0, 0, 0 }
#define DEFINE_IDR(name) struct idr name = IDR_INIT(name)
#define IDA_INIT(name)  { { 0, 0, 0 } }
#define DEFINE_IDA(name) struct ida name = IDA_INIT(name)

void  idr_init(struct idr *idr);
void  idr_init_base(struct idr *idr, int base);
void  idr_destroy(struct idr *idr);
/* allocate the lowest free id in [start, end) (end<=0 means "no upper bound"). */
int   idr_alloc(struct idr *idr, void *ptr, int start, int end, gfp_t gfp);
void *idr_find(struct idr *idr, int id);
void *idr_remove(struct idr *idr, int id);
void *idr_replace(struct idr *idr, void *ptr, int id);
int   idr_is_empty(struct idr *idr);

void  ida_init(struct ida *ida);
void  ida_destroy(struct ida *ida);
int   ida_alloc(struct ida *ida, gfp_t gfp);
int   ida_alloc_range(struct ida *ida, unsigned int min, unsigned int max, gfp_t gfp);
void  ida_free(struct ida *ida, unsigned int id);

#ifdef __cplusplus
}
#endif

#endif /* _LINUXKPI_LINUX_IDR_H */

#ifndef _LKPI_IDR_EXTRA
#define _LKPI_IDR_EXTRA
#define idr_preload(gfp) do{}while(0)
#define idr_preload_end() do{}while(0)
int idr_for_each(struct idr*, int (*fn)(int,void*,void*), void*);
#define idr_for_each_entry(idr,entry,id) for(id=0; ((entry)=idr_find((idr),id))!=0 || id<(idr)->cap; id++) if((entry))
#endif

#ifndef _LKPI_IDA_MINMAX
#define _LKPI_IDA_MINMAX
static inline int ida_alloc_max(struct ida *ida, unsigned int max, gfp_t gfp){ return ida_alloc_range(ida,0,max,gfp); }
static inline int ida_alloc_min(struct ida *ida, unsigned int min, gfp_t gfp){ return ida_alloc_range(ida,min,0xffffffffu,gfp); }
#endif
