/*
 * linuxkpi/include/linux/cache.h — cache-line constants for the shim.
 */
#ifndef _LINUXKPI_LINUX_CACHE_H
#define _LINUXKPI_LINUX_CACHE_H

#include <linux/compiler.h>

#define L1_CACHE_SHIFT 6
#define L1_CACHE_BYTES (1 << L1_CACHE_SHIFT)
#define SMP_CACHE_BYTES L1_CACHE_BYTES
#define __read_mostly
#define __ro_after_init

#endif /* _LINUXKPI_LINUX_CACHE_H */
