/*
 * linuxkpi/include/linux/pm_runtime.h — runtime PM shim: the GPU is ALWAYS awake.
 *
 * NanOS has no runtime-PM core to autosuspend the device, so every "get" reports the device active
 * and every "put"/autosuspend/idle call is a no-op. get_sync returns 1 (>=0 = success, was active),
 * suspended() is always false. This matches the plan's "always-awake" model; intel_runtime_pm.c's
 * own wakeref refcount (the asserts that a GT access holds a wakeref) is preserved on top of this.
 */
#ifndef _LINUXKPI_LINUX_PM_RUNTIME_H
#define _LINUXKPI_LINUX_PM_RUNTIME_H

#include <linux/types.h>
#include <linux/pm.h>

struct device;

static inline int  pm_runtime_get_sync(struct device *dev)          { (void)dev; return 1; }
static inline int  pm_runtime_resume_and_get(struct device *dev)    { (void)dev; return 0; }
static inline int  pm_runtime_get_if_in_use(struct device *dev)     { (void)dev; return 1; }
static inline int  pm_runtime_get_if_active(struct device *dev)     { (void)dev; return 1; }
static inline void pm_runtime_get_noresume(struct device *dev)      { (void)dev; }
static inline int  pm_runtime_put(struct device *dev)               { (void)dev; return 0; }
static inline int  pm_runtime_put_sync(struct device *dev)          { (void)dev; return 0; }
static inline int  pm_runtime_put_autosuspend(struct device *dev)   { (void)dev; return 0; }
static inline int  pm_runtime_put_noidle(struct device *dev)        { (void)dev; return 0; }
static inline void pm_runtime_mark_last_busy(struct device *dev)    { (void)dev; }
static inline void pm_runtime_use_autosuspend(struct device *dev)   { (void)dev; }
static inline void pm_runtime_dont_use_autosuspend(struct device *dev) { (void)dev; }
static inline void pm_runtime_set_autosuspend_delay(struct device *dev, int d) { (void)dev; (void)d; }
static inline void pm_runtime_enable(struct device *dev)            { (void)dev; }
static inline void pm_runtime_disable(struct device *dev)           { (void)dev; }
static inline void pm_runtime_allow(struct device *dev)             { (void)dev; }
static inline void pm_runtime_forbid(struct device *dev)            { (void)dev; }
static inline int  pm_runtime_enabled(struct device *dev)           { (void)dev; return 1; }
static inline int  pm_runtime_suspended(struct device *dev)         { (void)dev; return 0; }
static inline void pm_runtime_no_callbacks(struct device *dev)      { (void)dev; }
static inline void pm_runtime_set_active(struct device *dev)        { (void)dev; }
static inline void pm_runtime_set_suspended(struct device *dev)     { (void)dev; }

#endif /* _LINUXKPI_LINUX_PM_RUNTIME_H */
