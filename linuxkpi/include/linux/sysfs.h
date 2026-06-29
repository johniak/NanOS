#ifndef _LKPI_SYSFS_H
#define _LKPI_SYSFS_H
struct kobject;
static inline int sysfs_create_link(struct kobject *k, struct kobject *t, const char *n){ (void)k;(void)t;(void)n; return 0; }
static inline void sysfs_remove_link(struct kobject *k, const char *n){ (void)k;(void)n; }
struct attribute { const char *name; unsigned short mode; };
struct attribute_group { const char *name; struct attribute **attrs; };
#endif
