#ifndef _LKPI_KOBJECT_H
#define _LKPI_KOBJECT_H
#include <linux/types.h>
struct kobject; struct kobj_uevent_env { char buf[512]; int buflen; };
enum kobject_action { KOBJ_ADD, KOBJ_REMOVE, KOBJ_CHANGE, KOBJ_MOVE, KOBJ_ONLINE, KOBJ_OFFLINE };
static inline int kobject_uevent(struct kobject *k, enum kobject_action a){ (void)k;(void)a; return 0; }
static inline int kobject_uevent_env(struct kobject *k, enum kobject_action a, char *envp[]){ (void)k;(void)a;(void)envp; return 0; }
static inline int add_uevent_var(struct kobj_uevent_env *e, const char *fmt, ...){ (void)e;(void)fmt; return 0; }
static inline char *kobject_name(const struct kobject *k){ return 0; }
#endif
