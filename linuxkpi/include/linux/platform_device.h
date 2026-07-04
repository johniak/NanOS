#ifndef _LKPI_PLATFORM_DEVICE_H
#define _LKPI_PLATFORM_DEVICE_H
#include <linux/device.h>
struct platform_device { struct device dev; const char *name; int id; };
static inline int dev_is_platform(struct device *d){ (void)d; return 0; }
static inline struct platform_device *to_platform_device(struct device *d){ return container_of(d, struct platform_device, dev); }
static inline void *platform_get_drvdata(struct platform_device *p){ return dev_get_drvdata(&p->dev); }
static inline void platform_set_drvdata(struct platform_device *p, void *data){ dev_set_drvdata(&p->dev, data); }
struct resource;
/* descriptor for platform_device_register_full (i915 LPE audio child device). Registration is inert. */
struct platform_device_info {
	struct device *parent;
	struct fwnode_handle *fwnode;
	const char *name;
	int id;
	const struct resource *res;
	unsigned int num_res;
	const void *data;
	size_t size_data;
	unsigned long long dma_mask;
};
static inline struct platform_device *platform_device_register_full(const struct platform_device_info *info){ (void)info; return 0; }
static inline void platform_device_unregister(struct platform_device *p){ (void)p; }
static inline struct resource *platform_get_resource(struct platform_device *p, unsigned int type, unsigned int num){ (void)p;(void)type;(void)num; return 0; }
static inline int platform_get_irq(struct platform_device *p, unsigned int num){ (void)p;(void)num; return 0; }
#endif
