/* linuxkpi/include/asm/cacheflush.h — clflush_cache_range: flush a CPU cache range so a subsequent
 * GPU access sees the writes (real hardware coherency; on QEMU virtio it is harmless). clflush() +
 * the cacheline size come from the shim. */
#ifndef _LKPI_ASM_CACHEFLUSH_H
#define _LKPI_ASM_CACHEFLUSH_H
#include <linux/set_memory.h>   /* clflush() */
#include <asm/cpufeature.h>     /* boot_cpu_data.x86_clflush_size */
static inline void clflush_cache_range(void *addr, unsigned int size)
{
	unsigned int cl = boot_cpu_data.x86_clflush_size ? boot_cpu_data.x86_clflush_size : 64;
	char *p = (char *)((unsigned long)addr & ~(unsigned long)(cl - 1));
	char *end = (char *)addr + size;
	__asm__ __volatile__("mfence" ::: "memory");
	for (; p < end; p += cl) clflush((volatile void *)p);
	__asm__ __volatile__("mfence" ::: "memory");
}
#endif
