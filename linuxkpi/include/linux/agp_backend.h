/*
 * linuxkpi/include/linux/agp_backend.h — the AGP user-memory type flags i915's legacy GMCH path
 * (intel_ggtt_gmch.c) tags cache-mode with. GMCH is Gen5-and-older; the Dell's Gen9.5 uses the
 * modern GGTT, so this file exists only to let the unconditional i915-y object compile. Values match
 * Linux's <linux/agp_backend.h>.
 */
#ifndef _LKPI_LINUX_AGP_BACKEND_H
#define _LKPI_LINUX_AGP_BACKEND_H

#define AGP_USER_TYPES          (1 << 16)
#define AGP_USER_MEMORY         (AGP_USER_TYPES)
#define AGP_USER_CACHED_MEMORY  (AGP_USER_TYPES + 1)

#endif /* _LKPI_LINUX_AGP_BACKEND_H */
