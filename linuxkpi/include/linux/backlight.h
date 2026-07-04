/* linuxkpi/include/linux/backlight.h — backlight class. STUB (Dell panel backlight follow-on). */
#ifndef _LINUXKPI_LINUX_BACKLIGHT_H
#define _LINUXKPI_LINUX_BACKLIGHT_H
#include <linux/types.h>
struct device;
enum backlight_type { BACKLIGHT_RAW = 1, BACKLIGHT_PLATFORM, BACKLIGHT_FIRMWARE };
struct backlight_properties { int brightness; int max_brightness; int power; unsigned type; unsigned scale; unsigned int state; };
#ifndef BACKLIGHT_POWER_OFF
#define BACKLIGHT_POWER_ON       0
#define BACKLIGHT_POWER_OFF      4
#define BACKLIGHT_POWER_REDUCED  1
#define BL_CORE_SUSPENDED        (1<<0)
#define BL_CORE_FBBLANK          (1<<1)
#endif
struct backlight_device { struct backlight_properties props; void *data; };
struct backlight_ops { unsigned options; int (*update_status)(struct backlight_device *);
	int (*get_brightness)(struct backlight_device *); };
static inline struct backlight_device *backlight_device_register(const char *name, struct device *dev,
	void *devdata, const struct backlight_ops *ops, const struct backlight_properties *props){ (void)name;(void)dev;(void)devdata;(void)ops;(void)props; return 0; }
static inline struct backlight_device *devm_backlight_device_register(struct device *dev, const char *name,
	struct device *parent, void *devdata, const struct backlight_ops *ops, const struct backlight_properties *props){ (void)dev;(void)name;(void)parent;(void)devdata;(void)ops;(void)props; return 0; }
static inline void backlight_device_unregister(struct backlight_device *bd){ (void)bd; }
static inline struct backlight_device *backlight_device_get_by_name(const char *name){ (void)name; return 0; }
static inline void *bl_get_data(struct backlight_device *bd){ return bd ? bd->data : 0; }
#endif
