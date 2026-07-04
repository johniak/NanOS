#ifndef _LKPI_TIMER_H
#define _LKPI_TIMER_H
#include <linux/types.h>
#include <linux/list.h>
#ifdef __cplusplus
extern "C" {
#endif
/* lkpi_linked (private): 1 while this timer is on kpi_kthread.c's deadline list. `flags` keeps its
 * Linux meaning (passed through timer_setup) and is NOT overloaded for the linked bit. */
struct timer_list { struct list_head entry; unsigned long expires; void (*function)(struct timer_list*); unsigned long flags; int lkpi_linked; };
#define from_timer(var,callback_timer,timer_fieldname) container_of(callback_timer, __typeof__(*var), timer_fieldname)
#define timer_setup(t,fn,fl) do{ (t)->function=(fn); (t)->flags=(fl); INIT_LIST_HEAD(&(t)->entry); (t)->lkpi_linked=0; }while(0)
/* Real deadline timers (kpi_kthread.c): a timer thread fires `function` once `expires` (jiffies=ms)
 * is reached. mod_timer arms/re-arms; del_timer(_sync) disarms. */
int  mod_timer(struct timer_list *t, unsigned long expires);
void add_timer(struct timer_list *t);
int  del_timer(struct timer_list *t);
int  del_timer_sync(struct timer_list *t);
int  timer_delete_sync(struct timer_list *t);
#ifdef __cplusplus
}
#endif
#endif
