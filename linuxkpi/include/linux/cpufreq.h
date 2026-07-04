/* linuxkpi/include/linux/cpufreq.h — intel_llc reads the CPU cpufreq policy to pick an LLC ring
 * ratio. NanOS exposes no cpufreq policy (CPU power is HWP-managed), so lookups return NULL and
 * i915 falls back to its default ratio. */
#ifndef _LINUXKPI_LINUX_CPUFREQ_H
#define _LINUXKPI_LINUX_CPUFREQ_H
struct cpufreq_cpuinfo { unsigned int max_freq; unsigned int min_freq; unsigned int transition_latency; };
struct cpufreq_policy { struct cpufreq_cpuinfo cpuinfo; unsigned int cpuinfo_min_freq; unsigned int cpuinfo_max_freq; unsigned int cur; };
static inline struct cpufreq_policy *cpufreq_cpu_get(unsigned int cpu){ (void)cpu; return 0; }
static inline void cpufreq_cpu_put(struct cpufreq_policy *p){ (void)p; }
static inline unsigned int cpufreq_quick_get_max(unsigned int cpu){ (void)cpu; return 0; }
#endif
