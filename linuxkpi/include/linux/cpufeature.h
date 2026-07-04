/* linuxkpi/include/linux/cpufeature.h — forwards to the shim's asm/cpufeature.h (feature bits +
 * static_cpu_has/boot_cpu_has, all reporting absent: the kext is built -mno-sse and the SSE4.1
 * i915_memcpy fast path must fall back to plain copies). */
#ifndef _LINUXKPI_LINUX_CPUFEATURE_H
#define _LINUXKPI_LINUX_CPUFEATURE_H
#include <asm/cpufeature.h>
#endif
