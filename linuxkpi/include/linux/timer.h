#ifndef _LKPI_TIMER_H
#define _LKPI_TIMER_H
#include <linux/types.h>
#include <linux/list.h>
struct timer_list { struct list_head entry; unsigned long expires; void (*function)(struct timer_list*); unsigned long flags; };
#define from_timer(var,callback_timer,timer_fieldname) container_of(callback_timer, __typeof__(*var), timer_fieldname)
#define timer_setup(t,fn,fl) do{ (t)->function=(fn); (t)->flags=(fl); }while(0)
static inline int mod_timer(struct timer_list *t, unsigned long e){ (void)t;(void)e; return 0; }
static inline int del_timer(struct timer_list *t){ (void)t; return 0; }
static inline int del_timer_sync(struct timer_list *t){ (void)t; return 0; }
static inline int timer_delete_sync(struct timer_list *t){ (void)t; return 0; }
static inline void add_timer(struct timer_list *t){ (void)t; }
#endif
