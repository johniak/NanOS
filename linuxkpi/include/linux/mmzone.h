/* linuxkpi/include/linux/mmzone.h — minimal: the page-order count TTM's pool array is sized by.
 * NanOS has no NUMA zones or buddy allocator exposed here; TTM is compiled but Gen9 i915 uses
 * GEM-shmem, not TTM pools, so only the compile-time constants are needed. */
#ifndef _LINUXKPI_LINUX_MMZONE_H
#define _LINUXKPI_LINUX_MMZONE_H
#include <linux/types.h>
#ifndef MAX_PAGE_ORDER
#define MAX_PAGE_ORDER 10
#endif
#ifndef MAX_ORDER
#define MAX_ORDER MAX_PAGE_ORDER
#endif
#ifndef NR_PAGE_ORDERS
#define NR_PAGE_ORDERS (MAX_PAGE_ORDER + 1)
#endif
static inline int numa_node_id(void) { return 0; }
#define NUMA_NO_NODE (-1)
#define first_online_node 0
#endif
