#ifndef _LKPI_SCHED_H
#define _LKPI_SCHED_H
#include <linux/types.h>
#include <linux/timer.h>   /* i915_utils.h reaches timer_pending() only through <linux/sched.h> */
#define TASK_RUNNING 0
#define TASK_INTERRUPTIBLE 1
#define TASK_UNINTERRUPTIBLE 2
#define TASK_NORMAL (TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)
struct task_struct { int pid; const char *comm; void *mm; void *knx; };
extern struct task_struct *lkpi_current;
#define current (lkpi_current)
static inline void schedule(void){ __asm__ __volatile__("pause"); }
static inline int signal_pending(struct task_struct *t){ (void)t; return 0; }
static inline int signal_pending_state(unsigned int state, struct task_struct *t){ (void)state;(void)t; return 0; }
static inline int fatal_signal_pending(struct task_struct *t){ (void)t; return 0; }
static inline void set_current_state(int s){ (void)s; }
static inline void __set_current_state(int s){ (void)s; }
#define cond_resched() 0
#define MAX_SCHEDULE_TIMEOUT (~0L>>1)
static inline long schedule_timeout(long t){ return t; }
static inline long io_schedule_timeout(long t){ return t; }
#define TASK_COMM_LEN 16
#endif

#ifndef _LKPI_SCHED_EXTRA
#define _LKPI_SCHED_EXTRA
#include <linux/string.h>
static inline char *get_task_comm(char *buf, struct task_struct *t){ (void)t; buf[0]=0; return buf; }
#endif

#ifndef _LKPI_SCHED_EXTRA2
#define _LKPI_SCHED_EXTRA2
static inline int capable(int cap){ (void)cap; return 1; }
static inline int task_pid_nr(struct task_struct *t){ (void)t; return 0; }
static inline int task_tgid_nr(struct task_struct *t){ (void)t; return 0; }
static inline void *task_tgid(struct task_struct *t){ (void)t; return 0; }
#endif

#ifndef _LKPI_SCHED_X3
#define _LKPI_SCHED_X3
static inline int task_pid_vnr(struct task_struct *t){ (void)t; return 0; }
#endif

#ifndef _LKPI_SCHED_WAKE
#define _LKPI_SCHED_WAKE
static inline int wake_up_process(struct task_struct *t){ (void)t; return 0; }
#endif

#ifndef _LKPI_SCHED_TIMEOUT
#define _LKPI_SCHED_TIMEOUT
static inline long schedule_timeout_uninterruptible(long t){ (void)t; return 0; }
static inline long schedule_timeout_interruptible(long t){ (void)t; return 0; }
/* cond_resched_lock: drop the lock, (would) yield, retake it. The deferred-preemption scheduler never
 * preempts kernel readers, so there is nothing to yield to here — just report "did not resched". */
#define cond_resched_lock(lock) ({ (void)(lock); 0; })
#endif
