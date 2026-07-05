#ifndef _LKPI_ASM_CPUFEATURE_H
#define _LKPI_ASM_CPUFEATURE_H
struct cpuinfo_x86 { unsigned x86_clflush_size; unsigned x86_cache_alignment; unsigned char x86; };
extern struct cpuinfo_x86 boot_cpu_data;
#define X86_FEATURE_CLFLUSH 0
#define X86_FEATURE_XMM4_1 0
#define X86_FEATURE_HYPERVISOR 0
#define X86_FEATURE_CLWB 0
static inline int static_cpu_has(int f){ (void)f; return 0; }
static inline int boot_cpu_has(int f){ (void)f; return 0; }
static inline int cpu_feature_enabled(int f){ (void)f; return 0; }
#endif
