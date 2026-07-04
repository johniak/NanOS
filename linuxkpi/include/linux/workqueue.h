#ifndef _LKPI_WORKQUEUE_H
#define _LKPI_WORKQUEUE_H
#include <linux/types.h>
#include <linux/list.h>
#include <linux/timer.h>
#include <linux/interrupt.h>   /* i915 engine/scheduler types embed a tasklet_struct by value, reached
                                * only transitively (intel_engine_types.h -> here) — same routing as
                                * seqcount_t via timer.h. interrupt.h pulls only types/irqreturn, no cycle. */
#ifdef __cplusplus
extern "C" {
#endif
struct work_struct; typedef void (*work_func_t)(struct work_struct*);
/* `pending` (private): 1 while this work is queued on a workqueue's pending list (kpi_kthread.c). */
struct work_struct { work_func_t func; struct list_head entry; volatile int pending; };
struct workqueue_struct;   /* opaque: full definition lives in kpi_kthread.c */
struct delayed_work { struct work_struct work; struct timer_list timer; struct workqueue_struct *wq; };
#define INIT_WORK(w,f)         do{ (w)->func=(f); INIT_LIST_HEAD(&(w)->entry); (w)->pending=0; }while(0)
#define INIT_DELAYED_WORK(w,f) do{ (w)->work.func=(f); INIT_LIST_HEAD(&(w)->work.entry); (w)->work.pending=0; (w)->wq=0; INIT_LIST_HEAD(&(w)->timer.entry); (w)->timer.lkpi_linked=0; }while(0)
#define INIT_WORK_ONSTACK(w,f) INIT_WORK(w,f)

/* Async workqueues (kpi_kthread.c): each queue has one worker kthread (spawned once the scheduler is
 * up); before that, and under a forced LKPI_WQ_INLINE build, queue_work runs the work inline. */
bool schedule_work(struct work_struct *w);
bool queue_work(struct workqueue_struct *q, struct work_struct *w);
bool schedule_delayed_work(struct delayed_work *w, unsigned long delay);
bool queue_delayed_work(struct workqueue_struct *q, struct delayed_work *w, unsigned long delay);
bool mod_delayed_work(struct workqueue_struct *q, struct delayed_work *w, unsigned long delay);
bool cancel_work_sync(struct work_struct *w);
bool cancel_delayed_work_sync(struct delayed_work *w);
bool cancel_delayed_work(struct delayed_work *w);
void flush_work(struct work_struct *w);
void flush_workqueue(struct workqueue_struct *q);
void destroy_workqueue(struct workqueue_struct *q);
struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags, int max_active, ...);
struct workqueue_struct *create_singlethread_workqueue(const char *name);
struct work_struct *current_work(void);
extern struct workqueue_struct *system_wq, *system_unbound_wq, *system_long_wq, *system_highpri_wq;

/* One-time set-up (system queues + pump hook + deferred worker spawn). Call from the kext entry
 * BEFORE the driver probe. lkpi_wq_drain runs pending work (the cooperative wait pump calls it). */
void lkpi_wq_init(void);
void lkpi_wq_drain(void);
void lkpi_run_timers(void);   /* fire due timers now (timer thread loops on this) */
void lkpi_set_wq_pump(void (*fn)(void));   /* kpi_fence.c: register the wait-pump drain hook */

#define WQ_UNBOUND       0
#define WQ_MEM_RECLAIM   0
#define WQ_HIGHPRI       0
#define WQ_FREEZABLE     0
#define WQ_CPU_INTENSIVE 0
#define WQ_SYSFS         0
#define work_pending(w) ((w)->pending != 0)
#define to_delayed_work(w) container_of(w, struct delayed_work, work)
#define destroy_work_on_stack(w) do{}while(0)
#define destroy_delayed_work_on_stack(w) do{}while(0)
#define drain_workqueue(q) flush_workqueue(q)
#ifdef __cplusplus
}
#endif
bool flush_delayed_work(struct delayed_work *dw);
/* ordered workqueue == single in-flight work: alloc_workqueue with max_active=1. */
#define __WQ_ORDERED 0
#define alloc_ordered_workqueue(fmt, flags, ...) alloc_workqueue((fmt), (flags), 1, ##__VA_ARGS__)
#define delayed_work_pending(w) work_pending(&(w)->work)
/* non-sync cancel: the shim runs work either inline or on a single worker, so cancel == cancel_sync. */
#define cancel_work(w) cancel_work_sync(w)
#endif
