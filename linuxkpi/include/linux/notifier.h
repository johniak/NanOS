#ifndef _LKPI_NOTIFIER_H
#define _LKPI_NOTIFIER_H
#include <linux/types.h>
struct notifier_block; typedef int (*notifier_fn_t)(struct notifier_block*, unsigned long, void*);
struct notifier_block { notifier_fn_t notifier_call; struct notifier_block *next; int priority; };
struct blocking_notifier_head { struct notifier_block *head; };
struct atomic_notifier_head { struct notifier_block *head; };
static inline int blocking_notifier_chain_register(struct blocking_notifier_head *h, struct notifier_block *n){ (void)h;(void)n; return 0; }
static inline int blocking_notifier_chain_unregister(struct blocking_notifier_head *h, struct notifier_block *n){ (void)h;(void)n; return 0; }
static inline int blocking_notifier_call_chain(struct blocking_notifier_head *h, unsigned long v, void *p){ (void)h;(void)v;(void)p; return 0; }
static inline int atomic_notifier_chain_register(struct atomic_notifier_head *h, struct notifier_block *n){ (void)h;(void)n; return 0; }
static inline int atomic_notifier_chain_unregister(struct atomic_notifier_head *h, struct notifier_block *n){ (void)h;(void)n; return 0; }
static inline int atomic_notifier_call_chain(struct atomic_notifier_head *h, unsigned long v, void *p){ (void)h;(void)v;(void)p; return 0; }
/* one-time in-place init of an atomic notifier head (i915 uses it for reset notifiers). */
#define ATOMIC_INIT_NOTIFIER_HEAD(name) do { (name)->head = 0; } while (0)
#define BLOCKING_INIT_NOTIFIER_HEAD(name) do { (name)->head = 0; } while (0)
#define ATOMIC_NOTIFIER_HEAD(name) struct atomic_notifier_head name = { 0 }
#define BLOCKING_NOTIFIER_HEAD(name) struct blocking_notifier_head name = { 0 }
#define BLOCKING_NOTIFIER_INIT(name) { 0 }
#define NOTIFY_DONE 0
#define NOTIFY_OK   1
#endif
