#ifndef _LKPI_PLATFORM_DEVICE_H
#define _LKPI_PLATFORM_DEVICE_H
#include <linux/device.h>
struct platform_device { struct device dev; const char *name; int id; };
static inline int dev_is_platform(struct device *d){ (void)d; return 0; }
static inline struct platform_device *to_platform_device(struct device *d){ return container_of(d, struct platform_device, dev); }
static inline void *platform_get_drvdata(struct platform_device *p){ return dev_get_drvdata(&p->dev); }
#endif
