/* linuxkpi/include/linux/device/bus.h — bus_type + notifier surface (i915 HuC hooks the GSC bus's
 * notifier chain to defer firmware load). NanOS has no device-bus notifier chain, so register/
 * unregister succeed but never fire — the HuC delayed-load path just doesn't get a callback. */
#ifndef _LKPI_LINUX_DEVICE_BUS_H
#define _LKPI_LINUX_DEVICE_BUS_H
#include <linux/device.h>

struct notifier_block;
static inline int bus_register_notifier(const struct bus_type *bus, struct notifier_block *nb){ (void)bus;(void)nb; return 0; }
static inline int bus_unregister_notifier(const struct bus_type *bus, struct notifier_block *nb){ (void)bus;(void)nb; return 0; }
static inline int bus_register(const struct bus_type *bus){ (void)bus; return 0; }
static inline void bus_unregister(const struct bus_type *bus){ (void)bus; }
#define BUS_NOTIFY_ADD_DEVICE       0x00000001
#define BUS_NOTIFY_DEL_DEVICE       0x00000002
#define BUS_NOTIFY_BOUND_DRIVER     0x00000003
#define BUS_NOTIFY_UNBOUND_DRIVER   0x00000005
#define BUS_NOTIFY_BIND_DRIVER      0x00000004
#define BUS_NOTIFY_UNBIND_DRIVER    0x00000006
#define BUS_NOTIFY_DRIVER_NOT_BOUND 0x00000007

#endif /* _LKPI_LINUX_DEVICE_BUS_H */
