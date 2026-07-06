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
/* deferred-preemption kernel: nothing marks a reschedule request during a kernel section. */
static inline int need_resched(void){ return 0; }
static inline void set_current_state(int s){ (void)s; }
static inline void __set_current_state(int s){ (void)s; }
#define cond_resched() 0
/* LONG_MAX. Must shift the UNSIGNED all-ones then cast: `~0L>>1` is an ARITHMETIC shift of the
 * signed -1L and stays -1, which i915's wait_moving_fence returns verbatim as a bogus errno
 * (the Dell GGTT-scratch -EPERM). Every infinite wait (dma_fence_wait / dma_resv_wait_timeout with
 * MAX_SCHEDULE_TIMEOUT) that error-checks its result depends on this being positive. */
#define MAX_SCHEDULE_TIMEOUT ((long)(~0UL >> 1))
/* Honest cooperative timeout. i915_request_wait_timeout (i915_request.c:92) loops
 * `timeout = io_schedule_timeout(timeout)` and breaks only when the fence signals or timeout hits 0.
 * The old no-op (`return t`) never decremented AND never pumped, so if the completion breadcrumb MSI
 * never arrives the first request wait (park barrier / __engines_record_defaults) busy-spins FOREVER
 * = silent hang. Instead: pump the deferred sources (timers -> retire/heartbeat, workqueue drain,
 * fence poll) and burn ~1 real jiffy, then decrement by the REAL time elapsed (timeout is denominated
 * in jiffies = 1 ms; a bare pump iteration is ~µs, so decrementing per-iteration would expire honest
 * waits ~1000x too early). A finite timeout therefore actually EXPIRES -> i915 gets -ETIME, wedges the
 * GT itself, and probe finishes in degraded mode with a COMPLETE log instead of hanging. An infinite
 * wait (MAX_SCHEDULE_TIMEOUT) never expires but now PUMPS, so deferred work can still make progress. */
static inline long schedule_timeout(long t){
	extern void lkpi_wait_pump(void);
	extern unsigned long lkpi_jiffies(void);
	unsigned long start = lkpi_jiffies();
	lkpi_wait_pump();
	while ((long)(lkpi_jiffies() - start) < 1) __asm__ __volatile__("pause");   /* >= 1 jiffy real */
	if (t == MAX_SCHEDULE_TIMEOUT) return t;
	{ long used = (long)(lkpi_jiffies() - start); return t > used ? t - used : 0; }
}
static inline long io_schedule_timeout(long t){ return schedule_timeout(t); }
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
/* yield(): give up the CPU to another runnable task. i2c-algo-bit spins on it while waiting for the
 * bus line to settle. Under deferred preemption a kernel thread yields cooperatively; a pause hint
 * is the honest minimum (the bus-timing loop also bounds its own retries). */
static inline void yield(void){ __asm__ __volatile__("pause"); }
#endif
