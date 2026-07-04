/* linuxkpi/include/linux/pfn.h — page-frame-number helpers (identity-mapped: pfn == phys>>PAGE_SHIFT). */
#ifndef _LINUXKPI_LINUX_PFN_H
#define _LINUXKPI_LINUX_PFN_H
#include <linux/types.h>
#include <linux/const.h>
#ifndef PAGE_SHIFT
#define PAGE_SHIFT 12
#endif
#ifndef PAGE_SIZE
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#endif
#define PFN_ALIGN(x)   (((unsigned long)(x) + (PAGE_SIZE-1)) & ~((unsigned long)(PAGE_SIZE-1)))
#define PFN_UP(x)      (((x) + PAGE_SIZE-1) >> PAGE_SHIFT)
#define PFN_DOWN(x)    ((x) >> PAGE_SHIFT)
#define PFN_PHYS(x)    ((phys_addr_t)(x) << PAGE_SHIFT)
#define PHYS_PFN(x)    ((unsigned long)((x) >> PAGE_SHIFT))
#endif
