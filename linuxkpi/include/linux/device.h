/*
 * linuxkpi/include/linux/device.h — a minimal Linux device model for the shim.
 *
 * We bypass bus matching (the module hand-builds its device and calls the driver probe),
 * so this provides just enough: struct device with drvdata, dev_name, the dev_*() log
 * helpers, and stub registration. struct device_driver/bus_type are present as opaque
 * shells so driver structs compile.
 */
#ifndef _LINUXKPI_LINUX_DEVICE_H
#define _LINUXKPI_LINUX_DEVICE_H

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/list.h>
/* <drm/drm_print.h> (pulled by nearly every i915 file) only forward-declares struct seq_file, yet
 * i915's debugfs show() handlers dereference m->private. drm_print.h includes THIS header, so routing
 * the full struct seq_file definition through here completes it everywhere it is used. */
#include <linux/seq_file.h>
#include <linux/compiler.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/atomic.h>

struct device;
struct device_driver;
struct bus_type;
struct fwnode_handle;
struct device_node;

struct kobject { const char *name; struct kobject *parent; };
struct class { const char *name; const struct attribute_group **dev_groups; char *(*devnode)(const struct device*, unsigned short*); };
struct device_type { const char *name; const struct attribute_group **groups; void (*release)(struct device*); char *(*devnode)(const struct device*, unsigned short*, unsigned*, unsigned*); };
struct component_ops { int (*bind)(struct device*, struct device*, void*); void (*unbind)(struct device*, struct device*, void*); };
/* runtime-PM state block. The shim doesn't do autosuspend, so is_suspended stays 0 (always awake). */
struct dev_pm_info { bool is_suspended; unsigned int disable_depth; void *driver_flags; };
struct device {
	struct dev_pm_info power;
	struct device *parent;
	const char *init_name;
	char name[48];
	void *driver_data;
	struct device_driver *driver;
	struct bus_type *bus;
	void (*release)(struct device *dev);
	struct fwnode_handle *fwnode;
	struct device_node *of_node;
	u64 *dma_mask;
	u64 coherent_dma_mask;
	void *platform_data;
	struct kobject kobj;
	dev_t devt;
	struct class *class;
	const struct device_type *type;
	const struct attribute_group **groups;
	void (*type_release)(struct device*);
};

struct dev_pm_ops;
struct device_driver {
	const char *name;
	struct bus_type *bus;
	struct module *owner;
	int (*probe)(struct device *dev);
	void (*remove)(struct device *dev);
	void (*shutdown)(struct device *dev);
	const struct dev_pm_ops *pm;
	const struct attribute_group **dev_groups;
};

struct bus_type {
	const char *name;
	int (*match)(struct device *dev, struct device_driver *drv);
	int (*uevent)(const struct device *dev, struct kobj_uevent_env *env);
	int (*probe)(struct device *dev);
	void (*remove)(struct device *dev);
	void (*shutdown)(struct device *dev);
	const struct dev_pm_ops *pm;
};

/* driver-model driver registration (no global registry — the kext bootstrap launches drivers
 * directly, so this is a no-op). bus_register/bus_unregister live in <linux/device/bus.h>. */
static inline int  driver_register(struct device_driver *drv){ (void)drv; return 0; }
static inline void driver_unregister(struct device_driver *drv){ (void)drv; }

/* device-tree device lookup: no DT on x86/NanOS, so nothing matches (drm_mipi_dsi host binding). */
struct device_node;
static inline struct device *bus_find_device_by_of_node(const struct bus_type *bus, const struct device_node *np){ (void)bus; (void)np; return 0; }
struct fwnode_handle;
static inline void device_set_node(struct device *dev, struct fwnode_handle *fwnode){ (void)dev; (void)fwnode; }
/* iterate a device's children: the shim tracks no device hierarchy, so visit none. */
static inline int device_for_each_child(struct device *dev, void *data, int (*fn)(struct device *, void *)){ (void)dev; (void)data; (void)fn; return 0; }

static inline const char *dev_name(const struct device *dev) {
	return (dev && dev->init_name) ? dev->init_name : dev->name;
}
static inline int dev_set_name(struct device *dev, const char *fmt, ...) {
	(void)fmt; dev->init_name = 0; return 0;
}
static inline void *dev_get_drvdata(const struct device *dev) { return dev->driver_data; }
static inline void *dev_get_platdata(const struct device *dev) { return dev->platform_data; }
static inline void  dev_set_drvdata(struct device *dev, void *data) { dev->driver_data = data; }

static inline void device_initialize(struct device *dev) { (void)dev; }
static inline int  device_add(struct device *dev) { (void)dev; return 0; }
static inline void device_del(struct device *dev) { (void)dev; }
static inline int  device_register(struct device *dev) { (void)dev; return 0; }
static inline void device_unregister(struct device *dev) { (void)dev; }
static inline struct device *get_device(struct device *dev) { return dev; }
static inline void put_device(struct device *dev) { (void)dev; }

/* dev_*() logging: prefix with the device name, route through printk. */
#define dev_printk(level, dev, fmt, ...) printk(fmt, ##__VA_ARGS__)
#define dev_emerg(dev, fmt, ...)  printk(fmt, ##__VA_ARGS__)
#define dev_crit(dev, fmt, ...)   printk(fmt, ##__VA_ARGS__)
#define dev_err(dev, fmt, ...)    printk(fmt, ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...)   printk(fmt, ##__VA_ARGS__)
#define dev_notice(dev, fmt, ...) printk(fmt, ##__VA_ARGS__)
#define dev_info(dev, fmt, ...)   printk(fmt, ##__VA_ARGS__)
#define dev_dbg(dev, fmt, ...)    do {} while (0)
#define dev_err_once(dev, fmt, ...)  printk(fmt, ##__VA_ARGS__)
#define dev_warn_once(dev, fmt, ...) printk(fmt, ##__VA_ARGS__)
#define dev_info_once(dev, fmt, ...) printk(fmt, ##__VA_ARGS__)
#define dev_err_probe(dev, err, fmt, ...) ({ printk(fmt, ##__VA_ARGS__); (err); })
/* _ratelimited variants: no rate limiter in the shim printk, so they just print (i915 uses them on
 * DP-aux and reg-access error paths). */
#define dev_err_ratelimited(dev, fmt, ...)   printk(fmt, ##__VA_ARGS__)
#define dev_warn_ratelimited(dev, fmt, ...)  printk(fmt, ##__VA_ARGS__)
#define dev_notice_ratelimited(dev, fmt, ...) printk(fmt, ##__VA_ARGS__)
#define dev_info_ratelimited(dev, fmt, ...)  printk(fmt, ##__VA_ARGS__)
#define dev_dbg_ratelimited(dev, fmt, ...)   do {} while (0)
#define dev_WARN(dev, fmt, ...)   printk(fmt, ##__VA_ARGS__)

/* devm_ managed allocations: the shim does not track them (leak on detach is acceptable
 * for the single long-lived GPU device); back them with the heap so frees still work. */
static inline void *devm_kzalloc(struct device *dev, size_t size, gfp_t gfp) { (void)dev; return kzalloc(size, gfp); }
static inline void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp) { (void)dev; return kmalloc(size, gfp); }
static inline void *devm_kcalloc(struct device *dev, size_t n, size_t size, gfp_t gfp) { (void)dev; return kcalloc(n, size, gfp); }
static inline void  devm_kfree(struct device *dev, void *p) { (void)dev; kfree(p); }

#endif /* _LINUXKPI_LINUX_DEVICE_H */

#ifndef _LKPI_DEVICE_EXTRA
#define _LKPI_DEVICE_EXTRA
static inline const char *dev_driver_string(const struct device *dev){ (void)dev; return "virtio_gpu"; }
static inline int dev_to_node(struct device *dev){ (void)dev; return -1; }
extern void *knx_map_mmio(unsigned int, unsigned int);
static inline void *devm_request_mem_region(struct device *d, unsigned long s, unsigned long n, const char *nm){ (void)d;(void)nm; return knx_map_mmio((unsigned)s,(unsigned)n); }
#endif

#ifndef _LKPI_DEVICE_REG
#define _LKPI_DEVICE_REG
static inline int device_is_registered(struct device *d){ (void)d; return 1; }
static inline int devm_add_action_or_reset(struct device *d, void (*action)(void*), void *data){ (void)d;(void)action;(void)data; return 0; }
static inline int devm_add_action(struct device *d, void (*action)(void*), void *data){ (void)d;(void)action;(void)data; return 0; }
#endif

#ifndef _LKPI_DEVICE_ATTR
#define _LKPI_DEVICE_ATTR
#include <linux/sysfs.h>
struct device_attribute { struct attribute attr; long (*show)(struct device*, struct device_attribute*, char*); long (*store)(struct device*, struct device_attribute*, const char*, unsigned long); };
struct class_attribute { struct attribute attr; long (*show)(const struct class*, const struct class_attribute*, char*); long (*store)(const struct class*, const struct class_attribute*, const char*, unsigned long); };
#define __ATTR(_name,_mode,_show,_store) { .attr={.name=#_name,.mode=_mode}, .show=_show, .store=_store }
#define __ATTR_RW(_name) __ATTR(_name, 0644, _name##_show, _name##_store)
#define __ATTR_RO(_name) __ATTR(_name, 0444, _name##_show, 0)
#define __ATTR_WO(_name) __ATTR(_name, 0200, 0, _name##_store)
#define DEVICE_ATTR(_name,_mode,_show,_store) struct device_attribute dev_attr_##_name = __ATTR(_name,_mode,_show,_store)
#define DEVICE_ATTR_RW(_name) struct device_attribute dev_attr_##_name = __ATTR_RW(_name)
#define DEVICE_ATTR_RO(_name) struct device_attribute dev_attr_##_name = __ATTR_RO(_name)
#define DEVICE_ATTR_WO(_name) struct device_attribute dev_attr_##_name = __ATTR_WO(_name)
#define CLASS_ATTR_RO(_name) struct class_attribute class_attr_##_name = __ATTR_RO(_name)
#define CLASS_ATTR_RW(_name) struct class_attribute class_attr_##_name = __ATTR_RW(_name)
static inline struct class *class_create(const char *name){ (void)name; static struct class c; return &c; }
static inline void class_destroy(struct class *c){ (void)c; }
static inline int class_create_file(struct class *c, const struct class_attribute *a){ (void)c;(void)a; return 0; }
static inline void class_remove_file(struct class *c, const struct class_attribute *a){ (void)c;(void)a; }
static inline struct device *kobj_to_dev(struct kobject *k){ return container_of(k, struct device, kobj); }
static inline int device_create_file(struct device *d, const struct device_attribute *a){ (void)d;(void)a; return 0; }
static inline void device_remove_file(struct device *d, const struct device_attribute *a){ (void)d;(void)a; }
static inline int device_add_group(struct device *d, const struct attribute_group *g){ (void)d;(void)g; return 0; }
static inline int component_add(struct device *d, const struct component_ops *o){ (void)d;(void)o; return 0; }
static inline void component_del(struct device *d, const struct component_ops *o){ (void)d;(void)o; }
/* device_link: express a supplier/consumer PM+probe ordering edge. The shim has no PM-runtime graph,
 * so this returns a non-NULL sentinel (callers only NULL-check it) and del is a no-op. */
struct device_link;
static inline struct device_link *device_link_add(struct device *consumer, struct device *supplier, unsigned int flags){ (void)consumer;(void)supplier;(void)flags; return (struct device_link *)consumer; }
static inline void device_link_del(struct device_link *link){ (void)link; }
static inline void device_link_remove(void *consumer, struct device *supplier){ (void)consumer;(void)supplier; }
static inline bool device_iommu_mapped(struct device *d){ (void)d; return false; }
static inline int device_create_bin_file(struct device *d, const void *attr){ (void)d;(void)attr; return 0; }
static inline void device_remove_bin_file(struct device *d, const void *attr){ (void)d;(void)attr; }
static inline void dev_pm_set_driver_flags(struct device *d, unsigned long flags){ (void)d;(void)flags; }
#define DL_FLAG_STATELESS         (1<<0)
#define DL_FLAG_PM_RUNTIME        (1<<1)
#define DL_FLAG_RPM_ACTIVE        (1<<2)
#define DPM_FLAG_NO_DIRECT_COMPLETE (1<<0)
#endif

#ifndef _LKPI_DEVICE_ATTR2
#define _LKPI_DEVICE_ATTR2
#define S_IRUGO 0444
#define S_IWUSR 0200
#define S_IRWXU 0700
struct class_attribute_string { struct class_attribute attr; char *str; };
long show_class_attr_string(const struct class*, const struct class_attribute*, char*);
#define CLASS_ATTR_STRING(_name,_mode,_str) struct class_attribute_string class_attr_##_name = { __ATTR(_name,_mode,(void*)show_class_attr_string,0), (char*)_str }
static inline struct fwnode_handle *dev_fwnode(const struct device *d){ (void)d; return 0; }
/* bus_register/bus_unregister + notifier surface (mirrors mainline's <linux/device.h> pulling in
 * <linux/device/bus.h>). Included last, after struct bus_type is complete. */
#include <linux/device/bus.h>
#endif
