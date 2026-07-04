/* linuxkpi/include/linux/pgtable.h — page-protection bits. pgprot_t + the pgprot_* accessors live in
 * the shim's mm.h/types.h; this adds the PAGE_KERNEL* names and __pgprot/pgprot_val that ttm_caching
 * and i915 reference. Values are nominal (NanOS is identity-mapped; caching is set via MTRR/PAT
 * elsewhere or left UC — see kpi_iomap.c). */
#ifndef _LINUXKPI_LINUX_PGTABLE_H
#define _LINUXKPI_LINUX_PGTABLE_H
#include <linux/types.h>
#ifndef __pgprot
#define __pgprot(x)      ((pgprot_t)(x))
#endif
#ifndef pgprot_val
#define pgprot_val(x)    ((unsigned long)(x))
#endif
#define PAGE_KERNEL      __pgprot(0)
#define PAGE_KERNEL_IO   __pgprot(0)
#define PAGE_KERNEL_NOCACHE __pgprot(0)
#define PAGE_NONE        __pgprot(0)
static inline pgprot_t pgprot_combine(pgprot_t a, pgprot_t b){ (void)b; return a; }
#endif
