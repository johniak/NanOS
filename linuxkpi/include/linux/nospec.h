#ifndef _LKPI_NOSPEC_H
#define _LKPI_NOSPEC_H
#define array_index_nospec(index, size) (index)
/* i915_perf reaches struct ctl_table/register_sysctl transitively in mainline; it includes nospec.h
 * directly, so route sysctl.h here (both are leaf headers pulled by i915_perf.c). */
#include <linux/sysctl.h>
#endif
