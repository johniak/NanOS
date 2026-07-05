/*
 * linuxkpi/include/linux/pm.h — device power-management ops types. NanOS keeps the GPU always
 * powered (no ACPI D-states / no system suspend during bring-up), so these types exist for the
 * driver's dev_pm_ops table to compile; the callbacks are never invoked by a PM core.
 */
#ifndef _LINUXKPI_LINUX_PM_H
#define _LINUXKPI_LINUX_PM_H

#include <linux/types.h>

struct device;

typedef struct pm_message { int event; } pm_message_t;
#define PM_EVENT_ON       0x0000
#define PM_EVENT_FREEZE   0x0001
#define PM_EVENT_SUSPEND  0x0002
#define PM_EVENT_HIBERNATE 0x0004
#define PM_EVENT_RESUME   0x0010

struct dev_pm_ops {
	int (*prepare)(struct device *dev);
	void (*complete)(struct device *dev);
	int (*suspend)(struct device *dev);
	int (*resume)(struct device *dev);
	int (*freeze)(struct device *dev);
	int (*thaw)(struct device *dev);
	int (*poweroff)(struct device *dev);
	int (*restore)(struct device *dev);
	int (*suspend_late)(struct device *dev);
	int (*resume_early)(struct device *dev);
	int (*freeze_late)(struct device *dev);
	int (*thaw_early)(struct device *dev);
	int (*poweroff_late)(struct device *dev);
	int (*restore_early)(struct device *dev);
	int (*suspend_noirq)(struct device *dev);
	int (*resume_noirq)(struct device *dev);
	int (*freeze_noirq)(struct device *dev);
	int (*thaw_noirq)(struct device *dev);
	int (*poweroff_noirq)(struct device *dev);
	int (*restore_noirq)(struct device *dev);
	int (*runtime_suspend)(struct device *dev);
	int (*runtime_resume)(struct device *dev);
	int (*runtime_idle)(struct device *dev);
};

#define SYSTEM_SLEEP_PM_OPS(susp, res) \
	.suspend = susp, .resume = res, .freeze = susp, .thaw = res, \
	.poweroff = susp, .restore = res
#define SET_SYSTEM_SLEEP_PM_OPS(susp, res)       SYSTEM_SLEEP_PM_OPS(susp, res)
#define LATE_SYSTEM_SLEEP_PM_OPS(susp, res) \
	.suspend_late = susp, .resume_early = res, .freeze_late = susp, \
	.thaw_early = res, .poweroff_late = susp, .restore_early = res
#define SET_LATE_SYSTEM_SLEEP_PM_OPS(susp, res)  LATE_SYSTEM_SLEEP_PM_OPS(susp, res)
#define NOIRQ_SYSTEM_SLEEP_PM_OPS(susp, res) \
	.suspend_noirq = susp, .resume_noirq = res, .freeze_noirq = susp, \
	.thaw_noirq = res, .poweroff_noirq = susp, .restore_noirq = res
#define SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(susp, res) NOIRQ_SYSTEM_SLEEP_PM_OPS(susp, res)
#define RUNTIME_PM_OPS(susp, res, idle) \
	.runtime_suspend = susp, .runtime_resume = res, .runtime_idle = idle
#define SET_RUNTIME_PM_OPS(susp, res, idle)      RUNTIME_PM_OPS(susp, res, idle)

#define DEFINE_SIMPLE_DEV_PM_OPS(name, susp, res) \
	const struct dev_pm_ops name = { SYSTEM_SLEEP_PM_OPS(susp, res) }
#define SIMPLE_DEV_PM_OPS(name, susp, res) \
	const struct dev_pm_ops name = { SYSTEM_SLEEP_PM_OPS(susp, res) }
#define pm_ptr(_ptr) (_ptr)
#define pm_sleep_ptr(_ptr) (_ptr)

/* Generic runtime-PM callbacks a bus can point its dev_pm_ops at when the device has none of its
 * own. drm_mipi_dsi's default host ops reference them. No runtime-PM engine here → success no-ops. */
struct device;
static inline int pm_generic_runtime_suspend(struct device *dev){ (void)dev; return 0; }
static inline int pm_generic_runtime_resume(struct device *dev){ (void)dev; return 0; }
static inline int pm_generic_runtime_idle(struct device *dev){ (void)dev; return 0; }
static inline int pm_generic_suspend(struct device *dev){ (void)dev; return 0; }
static inline int pm_generic_resume(struct device *dev){ (void)dev; return 0; }
static inline int pm_generic_freeze(struct device *dev){ (void)dev; return 0; }
static inline int pm_generic_thaw(struct device *dev){ (void)dev; return 0; }
static inline int pm_generic_poweroff(struct device *dev){ (void)dev; return 0; }
static inline int pm_generic_restore(struct device *dev){ (void)dev; return 0; }

#endif /* _LINUXKPI_LINUX_PM_H */
