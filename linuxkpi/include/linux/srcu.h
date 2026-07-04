#ifndef _LKPI_SRCU_H
#define _LKPI_SRCU_H
struct srcu_struct { int n; };
#define DEFINE_STATIC_SRCU(name) static struct srcu_struct name
static inline int srcu_read_lock(struct srcu_struct *s){ (void)s; return 0; }
static inline void srcu_read_unlock(struct srcu_struct *s, int i){ (void)s;(void)i; }
static inline void synchronize_srcu(struct srcu_struct *s){ (void)s; }
static inline void synchronize_srcu_expedited(struct srcu_struct *s){ (void)s; }
static inline int init_srcu_struct(struct srcu_struct *s){ (void)s; return 0; }
static inline void cleanup_srcu_struct(struct srcu_struct *s){ (void)s; }
#define srcu_dereference(p,s) (p)
#endif
