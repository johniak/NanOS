/* linuxkpi/include/linux/perf_event.h — CONFIG_PERF_EVENTS=n: i915_pmu.o is NOT built, but
 * i915_pmu.h (included by i915_drv.h) embeds `struct pmu base`, so the types must exist. They are
 * opaque placeholders — no PMU is ever registered. */
#ifndef _LINUXKPI_LINUX_PERF_EVENT_H
#define _LINUXKPI_LINUX_PERF_EVENT_H
#include <linux/types.h>
struct device;
struct perf_event { int dummy; };
struct pmu {
	unsigned int task_ctx_nr;
	const char *name;
	int type;
	int capabilities;
	void *attr_groups;
	int (*event_init)(struct perf_event *);
	void *reserved[8];
};
struct perf_event_attr { u64 config; u32 type; };
static inline int perf_pmu_register(struct pmu *p, const char *n, int t){ (void)p;(void)n;(void)t; return 0; }
static inline void perf_pmu_unregister(struct pmu *p){ (void)p; }
#endif
