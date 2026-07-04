#ifndef _LKPI_SYSFS_H
#define _LKPI_SYSFS_H
struct kobject;
static inline int sysfs_create_link(struct kobject *k, struct kobject *t, const char *n){ (void)k;(void)t;(void)n; return 0; }
static inline void sysfs_remove_link(struct kobject *k, const char *n){ (void)k;(void)n; }
struct attribute { const char *name; unsigned short mode; };
struct attribute_group { const char *name; struct attribute **attrs; struct bin_attribute **bin_attrs; unsigned short (*is_visible)(struct kobject*, struct attribute*, int); };
#endif

#ifndef _LKPI_SYSFS_X
#define _LKPI_SYSFS_X
#include <linux/types.h>
struct bin_attribute { struct attribute attr; size_t size; void *private;
  long (*read)(struct file*, struct kobject*, struct bin_attribute*, char*, long, unsigned long);
  long (*write)(struct file*, struct kobject*, struct bin_attribute*, char*, long, unsigned long);
  int (*mmap)(struct file*, struct kobject*, struct bin_attribute*, struct vm_area_struct*); };
#define __BIN_ATTR(_name,_mode,_read,_write,_size) { .attr={.name=#_name,.mode=_mode}, .size=_size, .read=_read, .write=_write }
#define BIN_ATTR_RO(_name,_size) struct bin_attribute bin_attr_##_name = __BIN_ATTR(_name,0444,_name##_read,0,_size)
struct attribute_group_x { const char *name; struct attribute **attrs; struct bin_attribute **bin_attrs; };
/* kobj_attribute: a sysfs attribute with kobject-scoped show/store (Linux keeps it in kobject.h; it
 * embeds struct attribute, defined here, so it lives with its base). i915 perf embeds one by value. */
#ifndef _LKPI_KOBJ_ATTRIBUTE
#define _LKPI_KOBJ_ATTRIBUTE
struct kobj_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kobject *kobj, struct kobj_attribute *attr, char *buf);
	ssize_t (*store)(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count);
};
#endif
int sysfs_streq(const char *a, const char *b);
long sysfs_emit(char *buf, const char *fmt, ...);
long sysfs_emit_at(char *buf, int at, const char *fmt, ...);
static inline int sysfs_create_bin_file(struct kobject *k, const struct bin_attribute *a){ (void)k;(void)a; return 0; }
static inline void sysfs_remove_bin_file(struct kobject *k, const struct bin_attribute *a){ (void)k;(void)a; }
#define ATTRIBUTE_GROUPS(name) static const struct attribute_group *name##_groups[] = { &name##_group, 0 }
#endif
