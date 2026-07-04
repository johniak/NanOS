#ifndef _LKPI_PID_H
#define _LKPI_PID_H
struct pid;
static inline struct pid *get_pid(struct pid *p){ return p; }
static inline void put_pid(struct pid *p){ (void)p; }
struct pid *task_pid(struct task_struct *t);
static inline struct pid *get_task_pid(struct task_struct *t, int type){ (void)type; return task_pid(t); }
enum pid_type { PIDTYPE_PID, PIDTYPE_TGID, PIDTYPE_PGID, PIDTYPE_SID, PIDTYPE_MAX };
static inline int pid_vnr(struct pid *p){ (void)p; return 0; }
static inline int pid_nr(struct pid *p){ (void)p; return 0; }
#endif
