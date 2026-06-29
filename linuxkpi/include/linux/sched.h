#ifndef _LKPI_SCHED_H
#define _LKPI_SCHED_H
#include <linux/types.h>
#define TASK_RUNNING 0
#define TASK_INTERRUPTIBLE 1
#define TASK_UNINTERRUPTIBLE 2
struct task_struct { int pid; const char *comm; void *mm; };
extern struct task_struct *lkpi_current;
#define current (lkpi_current)
static inline void schedule(void){ __asm__ __volatile__("pause"); }
static inline int signal_pending(struct task_struct *t){ (void)t; return 0; }
static inline void set_current_state(int s){ (void)s; }
static inline void __set_current_state(int s){ (void)s; }
#define cond_resched() 0
#define MAX_SCHEDULE_TIMEOUT (~0L>>1)
static inline long schedule_timeout(long t){ return t; }
static inline long io_schedule_timeout(long t){ return t; }
#define TASK_COMM_LEN 16
#endif
