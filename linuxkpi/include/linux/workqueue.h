#ifndef _LKPI_WORKQUEUE_H
#define _LKPI_WORKQUEUE_H
#include <linux/types.h>
#include <linux/list.h>
struct work_struct; typedef void (*work_func_t)(struct work_struct*);
struct work_struct { work_func_t func; struct list_head entry; };
struct delayed_work { struct work_struct work; };
struct workqueue_struct { int n; };
#define INIT_WORK(w,f) do{ (w)->func=(f); INIT_LIST_HEAD(&(w)->entry); }while(0)
#define INIT_DELAYED_WORK(w,f) do{ (w)->work.func=(f); }while(0)
#define INIT_WORK_ONSTACK(w,f) INIT_WORK(w,f)
static inline bool schedule_work(struct work_struct *w){ if(w->func)w->func(w); return true; }
static inline bool queue_work(struct workqueue_struct *q, struct work_struct *w){ (void)q; if(w->func)w->func(w); return true; }
static inline bool schedule_delayed_work(struct delayed_work *w, unsigned long d){ (void)d; if(w->work.func)w->work.func(&w->work); return true; }
static inline bool queue_delayed_work(struct workqueue_struct *q, struct delayed_work *w, unsigned long d){ (void)q;(void)d; if(w->work.func)w->work.func(&w->work); return true; }
static inline bool cancel_work_sync(struct work_struct *w){ (void)w; return false; }
static inline bool cancel_delayed_work_sync(struct delayed_work *w){ (void)w; return false; }
static inline bool cancel_delayed_work(struct delayed_work *w){ (void)w; return false; }
static inline void flush_work(struct work_struct *w){ (void)w; }
static inline void flush_workqueue(struct workqueue_struct *q){ (void)q; }
static inline void destroy_workqueue(struct workqueue_struct *q){ (void)q; }
static inline struct workqueue_struct *alloc_workqueue(const char *f, unsigned int fl, int m, ...){ (void)f;(void)fl;(void)m; static struct workqueue_struct wq; return &wq; }
static inline struct workqueue_struct *create_singlethread_workqueue(const char *n){ (void)n; static struct workqueue_struct wq; return &wq; }
#define WQ_UNBOUND 0
#define WQ_MEM_RECLAIM 0
#define work_pending(w) (false)
#define to_delayed_work(w) container_of(w, struct delayed_work, work)
#endif
