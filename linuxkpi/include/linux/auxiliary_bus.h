/* linuxkpi/include/linux/auxiliary_bus.h — auxiliary-bus device model for the i915 GSC/MEI split.
 * Gen9 (Comet Lake) has no GSC graphics controller, so registration is inert (no real aux bus). */
#ifndef _LINUXKPI_LINUX_AUXILIARY_BUS_H
#define _LINUXKPI_LINUX_AUXILIARY_BUS_H
#include <linux/device.h>

struct auxiliary_device {
	struct device dev;
	const char *name;
	int id;
};
struct auxiliary_driver {
	int (*probe)(struct auxiliary_device *, const void *id);
	void (*remove)(struct auxiliary_device *);
	const char *name;
};

static inline struct auxiliary_device *to_auxiliary_dev(struct device *dev){ return container_of(dev, struct auxiliary_device, dev); }
static inline int auxiliary_device_init(struct auxiliary_device *adev){ (void)adev; return 0; }
static inline int __auxiliary_device_add(struct auxiliary_device *adev, const char *modname){ (void)adev;(void)modname; return 0; }
#define auxiliary_device_add(adev) __auxiliary_device_add((adev), KBUILD_MODNAME)
static inline void auxiliary_device_uninit(struct auxiliary_device *adev){ (void)adev; }
static inline void auxiliary_device_delete(struct auxiliary_device *adev){ (void)adev; }
static inline void *auxiliary_get_drvdata(struct auxiliary_device *adev){ return dev_get_drvdata(&adev->dev); }
static inline void auxiliary_set_drvdata(struct auxiliary_device *adev, void *data){ dev_set_drvdata(&adev->dev, data); }

#endif /* _LINUXKPI_LINUX_AUXILIARY_BUS_H */
