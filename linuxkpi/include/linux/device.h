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
struct device {
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
	void (*type_release)(struct device*);
};

struct device_driver {
	const char *name;
	struct bus_type *bus;
	int (*probe)(struct device *dev);
	void (*remove)(struct device *dev);
};

struct bus_type {
	const char *name;
	int (*match)(struct device *dev, struct device_driver *drv);
	int (*probe)(struct device *dev);
};

static inline const char *dev_name(const struct device *dev) {
	return (dev && dev->init_name) ? dev->init_name : dev->name;
}
static inline int dev_set_name(struct device *dev, const char *fmt, ...) {
	(void)fmt; dev->init_name = 0; return 0;
}
static inline void *dev_get_drvdata(const struct device *dev) { return dev->driver_data; }
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
#endif
