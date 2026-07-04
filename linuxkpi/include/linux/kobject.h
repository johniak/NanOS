#ifndef _LKPI_KOBJECT_H
#define _LKPI_KOBJECT_H
#include <linux/types.h>
struct kobject; struct kobj_uevent_env { char buf[512]; int buflen; };
enum kobject_action { KOBJ_ADD, KOBJ_REMOVE, KOBJ_CHANGE, KOBJ_MOVE, KOBJ_ONLINE, KOBJ_OFFLINE };
static inline int kobject_uevent(struct kobject *k, enum kobject_action a){ (void)k;(void)a; return 0; }
static inline int kobject_uevent_env(struct kobject *k, enum kobject_action a, char *envp[]){ (void)k;(void)a;(void)envp; return 0; }
static inline int add_uevent_var(struct kobj_uevent_env *e, const char *fmt, ...){ (void)e;(void)fmt; return 0; }
static inline char *kobject_name(const struct kobject *k){ (void)k; return 0; }
/* create-and-add: allocate+register a kobject under `parent`. The shim doesn't build a sysfs tree,
 * so return a small heap kobject the caller can put() (i915 only NULL-checks and later kobject_put). */
struct kobject *kobject_create_and_add(const char *name, struct kobject *parent);
void kobject_put(struct kobject *k);
static inline struct kobject *kobject_get(struct kobject *k){ return k; }
static inline int kobject_init_and_add(struct kobject *k, const void *ktype, struct kobject *parent, const char *fmt, ...){ (void)k;(void)ktype;(void)parent;(void)fmt; return 0; }
static inline void kobject_del(struct kobject *k){ (void)k; }
#endif
