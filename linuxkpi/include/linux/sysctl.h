/* linuxkpi/include/linux/sysctl.h — i915_perf registers dev.i915 sysctl knobs (perf_stream_paranoid,
 * oa_max_sample_rate). NanOS has no /proc/sys, so registration is inert (returns a dummy header). */
#ifndef _LINUXKPI_LINUX_SYSCTL_H
#define _LINUXKPI_LINUX_SYSCTL_H
#include <linux/types.h>

struct ctl_table_header;
struct ctl_table {
	const char *procname;
	void *data;
	int maxlen;
	unsigned short mode;
	int (*proc_handler)(struct ctl_table *, int, void *, size_t *, loff_t *);
	void *extra1;
	void *extra2;
};

/* SYSCTL_ZERO/ONE are the shared &zero/&one bounds; any non-NULL sentinel suffices here. */
extern int sysctl_vals[];
#define SYSCTL_ZERO ((void *)&sysctl_vals[0])
#define SYSCTL_ONE  ((void *)&sysctl_vals[1])
#define SYSCTL_INT_MAX ((void *)&sysctl_vals[2])

static inline int proc_dointvec(struct ctl_table *t, int w, void *b, size_t *l, loff_t *p){ (void)t;(void)w;(void)b;(void)l;(void)p; return 0; }
static inline int proc_dointvec_minmax(struct ctl_table *t, int w, void *b, size_t *l, loff_t *p){ (void)t;(void)w;(void)b;(void)l;(void)p; return 0; }
static inline struct ctl_table_header *register_sysctl(const char *path, struct ctl_table *table){ (void)path;(void)table; return 0; }
static inline void unregister_sysctl_table(struct ctl_table_header *h){ (void)h; }

#endif /* _LINUXKPI_LINUX_SYSCTL_H */
