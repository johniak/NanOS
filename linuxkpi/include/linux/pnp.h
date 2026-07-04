/* linuxkpi/include/linux/pnp.h — minimal guard. i915's opregion/stolen code checks whether a PnP
 * range is reserved (ACPI motherboard resources); NanOS reserves none via PnP, so it returns 0. */
#ifndef _LKPI_LINUX_PNP_H
#define _LKPI_LINUX_PNP_H
static inline int pnp_range_reserved(unsigned long start, unsigned long end){ (void)start;(void)end; return 0; }
#endif
