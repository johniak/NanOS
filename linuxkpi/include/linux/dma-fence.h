#ifndef _LKPI_DMA_FENCE_H
#define _LKPI_DMA_FENCE_H
#include <linux/types.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
struct dma_fence;
struct dma_fence_ops;
struct dma_fence {
  spinlock_t *lock; const struct dma_fence_ops *ops;
  struct list_head cb_list; u64 context; u64 seqno; unsigned long flags;
  struct kref refcount; int error;
};
struct dma_fence_cb; typedef void (*dma_fence_func_t)(struct dma_fence*, struct dma_fence_cb*);
struct dma_fence_cb { struct list_head node; dma_fence_func_t func; };
struct dma_fence_ops {
  bool use_64bit_seqno;
  const char *(*get_driver_name)(struct dma_fence*);
  const char *(*get_timeline_name)(struct dma_fence*);
  bool (*enable_signaling)(struct dma_fence*);
  bool (*signaled)(struct dma_fence*);
  long (*wait)(struct dma_fence*, bool, long);
  void (*release)(struct dma_fence*);
  void (*fence_value_str)(struct dma_fence*, char*, int);
  void (*timeline_value_str)(struct dma_fence*, char*, int);
};
enum { DMA_FENCE_FLAG_SIGNALED_BIT=0, DMA_FENCE_FLAG_TIMESTAMP_BIT, DMA_FENCE_FLAG_ENABLE_SIGNAL_BIT, DMA_FENCE_FLAG_USER_BITS };
#ifdef __cplusplus
extern "C" {
#endif
void dma_fence_init(struct dma_fence*, const struct dma_fence_ops*, spinlock_t*, u64, u64);
void dma_fence_free(struct dma_fence*);
struct dma_fence *dma_fence_get(struct dma_fence*);
struct dma_fence *dma_fence_get_rcu(struct dma_fence*);
void dma_fence_put(struct dma_fence*);
int  dma_fence_signal(struct dma_fence*);
int  dma_fence_signal_locked(struct dma_fence*);
bool dma_fence_is_signaled(struct dma_fence*);
long dma_fence_wait_timeout(struct dma_fence*, bool, long);
long dma_fence_wait(struct dma_fence*, bool);
int  dma_fence_add_callback(struct dma_fence*, struct dma_fence_cb*, dma_fence_func_t);
u64  dma_fence_context_alloc(unsigned);
#ifdef __cplusplus
}
#endif
static inline bool dma_fence_is_signaled_locked(struct dma_fence*f){return dma_fence_is_signaled(f);}
#endif

#ifndef _LKPI_DMA_FENCE_EXTRA
#define _LKPI_DMA_FENCE_EXTRA
static inline bool dma_fence_is_later(struct dma_fence *a, struct dma_fence *b){ return a&&b&&(a->seqno>b->seqno); }
static inline bool dma_fence_match_context(struct dma_fence *f, u64 ctx){ return f && f->context==ctx; }
static inline struct dma_fence *dma_fence_get_rcu_safe(struct dma_fence **pf){ return pf?*pf:0; }
#endif

#ifndef _LKPI_DMA_FENCE_EXTRA2
#define _LKPI_DMA_FENCE_EXTRA2
static inline void dma_fence_set_deadline(struct dma_fence *f, ktime_t d){ (void)f;(void)d; }
#endif

#ifndef _LKPI_DMA_FENCE_STUB
#define _LKPI_DMA_FENCE_STUB
struct dma_fence *dma_fence_get_stub(void);
struct dma_fence *dma_fence_allocate_private_stub(ktime_t timestamp);
static inline int dma_fence_remove_callback(struct dma_fence *f, struct dma_fence_cb *cb){ (void)f;(void)cb; return 0; }
#endif
#ifndef _LKPI_FENCE_TS
#define _LKPI_FENCE_TS
static inline int dma_fence_signal_timestamp(struct dma_fence *f, ktime_t t){ (void)t; return dma_fence_signal(f); }
#endif

#ifndef _LKPI_FENCE_SETERR
#define _LKPI_FENCE_SETERR
static inline void dma_fence_set_error(struct dma_fence *f, int error){ if (f) f->error = error; }
#endif
