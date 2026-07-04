/* linuxkpi/include/linux/pfn_t.h — pfn_t wrapper (i915 stolen/lmem). Identity-mapped: a plain pfn. */
#ifndef _LINUXKPI_LINUX_PFN_T_H
#define _LINUXKPI_LINUX_PFN_T_H
#include <linux/pfn.h>
#include <linux/mm.h>
typedef struct { unsigned long val; } pfn_t;
#define PFN_DEV (1UL << (BITS_PER_LONG-4))
#define PFN_MAP (1UL << (BITS_PER_LONG-2))
static inline pfn_t __pfn_to_pfn_t(unsigned long pfn, unsigned long flags){ pfn_t p = { pfn | flags }; return p; }
static inline pfn_t pfn_to_pfn_t(unsigned long pfn){ return __pfn_to_pfn_t(pfn, 0); }
static inline unsigned long pfn_t_to_pfn(pfn_t p){ return p.val & ~(PFN_DEV|PFN_MAP); }
#endif
