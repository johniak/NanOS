/*
 * linuxkpi/include/linux/shrinker.h — shrinker registry (kpi_shrinker.c).
 *
 * i915 registers a shrinker so the kernel can reclaim purgeable GEM pages under memory pressure.
 * NanOS does not yet drive reclaim from its allocator, so the registry is REAL (allocated,
 * linked, freed correctly) but is never invoked — a boot notice states this. i915 bring-up on an
 * 8-16 GiB machine does not depend on reclaim; wiring the OOM path to call these is a recorded
 * follow-on (see docs/en/linuxkpi.md).
 */
#ifndef _LINUXKPI_LINUX_SHRINKER_H
#define _LINUXKPI_LINUX_SHRINKER_H

#include <linux/types.h>
#include <linux/list.h>

#ifdef __cplusplus
extern "C" {
#endif

/* count_objects return sentinels + default cost. */
#define SHRINK_STOP   (~0UL)
#define SHRINK_EMPTY  (~0UL - 1)
#define DEFAULT_SEEKS 2

/* shrinker_alloc flags. */
#define SHRINKER_NUMA_AWARE   (1 << 0)
#define SHRINKER_MEMCG_AWARE  (1 << 1)
#define SHRINKER_NONSLAB      (1 << 2)

struct mem_cgroup;

struct shrink_control {
	gfp_t gfp_mask;
	int nid;
	unsigned long nr_to_scan;
	unsigned long nr_scanned;
	struct mem_cgroup *memcg;
};

struct shrinker {
	unsigned long (*count_objects)(struct shrinker *, struct shrink_control *sc);
	unsigned long (*scan_objects)(struct shrinker *, struct shrink_control *sc);
	long batch;
	int seeks;
	unsigned int flags;
	void *private_data;
	struct list_head list;   /* linked into the shim's registry by shrinker_register */
};

/* 6.12 API: allocate (name is fmt+va, ignored here), fill callbacks, then register. */
struct shrinker *shrinker_alloc(unsigned int flags, const char *fmt, ...);
void shrinker_register(struct shrinker *shrinker);
void shrinker_free(struct shrinker *shrinker);

/* Test/introspection hook (not Linux API): count of currently-registered shrinkers. */
int lkpi_shrinker_registered_count(void);

#ifdef __cplusplus
}
#endif

#endif /* _LINUXKPI_LINUX_SHRINKER_H */
