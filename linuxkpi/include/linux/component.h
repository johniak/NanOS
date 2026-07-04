/* linuxkpi/include/linux/component.h — component aggregator surface for the shim.
 * i915 registers "component" endpoints (GSC proxy, PXP-TEE, audio, HDCP) that in the full kernel
 * bind against a matching master (mei, snd_hda). NanOS has no such masters, so add/del succeed but
 * never bind — the dependent features stay dormant, which is exactly the intended minimal build.
 *
 * struct component_ops + component_add/component_del already live in <linux/device.h> (reached
 * everywhere); we pull that and only add the pieces device.h lacks, so a TU including both is fine. */
#ifndef _LINUXKPI_LINUX_COMPONENT_H
#define _LINUXKPI_LINUX_COMPONENT_H

#include <linux/device.h>

struct component_master_ops { int (*bind)(struct device*); void (*unbind)(struct device*); };
struct component_match;

static inline int component_add_typed(struct device *dev, const struct component_ops *ops, int subcomponent){ (void)dev;(void)ops;(void)subcomponent; return 0; }
static inline int component_bind_all(struct device *dev, void *data){ (void)dev;(void)data; return 0; }
static inline void component_unbind_all(struct device *dev, void *data){ (void)dev;(void)data; }
static inline int component_master_add_with_match(struct device *dev, const struct component_master_ops *ops, struct component_match *match){ (void)dev;(void)ops;(void)match; return 0; }
static inline void component_master_del(struct device *dev, const struct component_master_ops *ops){ (void)dev;(void)ops; }
static inline void component_match_add_release(struct device *dev, struct component_match **matchptr, void (*release)(struct device*, void*), int (*compare)(struct device*, void*), void *data){ (void)dev;(void)matchptr;(void)release;(void)compare;(void)data; }

#endif /* _LINUXKPI_LINUX_COMPONENT_H */
