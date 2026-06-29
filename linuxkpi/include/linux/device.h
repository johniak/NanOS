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
extern void *knx_map_mmio(unsigned int, unsigned int);
static inline void *devm_request_mem_region(struct device *d, unsigned long s, unsigned long n, const char *nm){ (void)d;(void)nm; return knx_map_mmio((unsigned)s,(unsigned)n); }
#endif
