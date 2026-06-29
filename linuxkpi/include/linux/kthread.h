#ifndef _LKPI_KTHREAD_H
#define _LKPI_KTHREAD_H
#include <linux/types.h>
#include <linux/list.h>
struct kthread_work; typedef void (*kthread_work_func_t)(struct kthread_work*);
struct kthread_work { kthread_work_func_t func; struct list_head node; };
struct task_struct;
struct kthread_worker { int n; struct task_struct *task; };
#define KTHREAD_WORKER_INIT(name) { 0 }
static inline void kthread_init_work(struct kthread_work *w, kthread_work_func_t f){ w->func=f; }
static inline void kthread_init_worker(struct kthread_worker *w){ (void)w; }
static inline bool kthread_queue_work(struct kthread_worker *w, struct kthread_work *work){ (void)w; if(work->func)work->func(work); return true; }
static inline void kthread_flush_work(struct kthread_work *w){ (void)w; }
static inline void kthread_flush_worker(struct kthread_worker *w){ (void)w; }
static inline struct kthread_worker *kthread_create_worker(unsigned flags, const char *name, ...){ (void)flags;(void)name; static struct kthread_worker kw; return &kw; }
static inline void kthread_destroy_worker(struct kthread_worker *w){ (void)w; }
static inline struct task_struct *kthread_run(int (*fn)(void*), void *arg, const char *name, ...){ (void)fn;(void)arg;(void)name; return 0; }
static inline int kthread_stop(struct task_struct *t){ (void)t; return 0; }
static inline bool kthread_should_stop(void){ return false; }
static inline bool kthread_cancel_work_sync(struct kthread_work *w){ (void)w; return false; }
static inline bool kthread_cancel_delayed_work_sync(void *w){ (void)w; return false; }
/* scheduler policy hint — NanOS kthreads run at the default priority; no-op. */
static inline void sched_set_fifo(struct task_struct *t){ (void)t; }
static inline void sched_set_fifo_low(struct task_struct *t){ (void)t; }
#endif
