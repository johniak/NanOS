/*
 * linuxkpi/include/linux/gfp.h — allocation flags for the NanOS LinuxKPI shim.
 * The NanOS kernel heap is a single context (no atomic/DMA zones), so the only flag we
 * act on is __GFP_ZERO; the rest are accepted and ignored.
 */
#ifndef _LINUXKPI_LINUX_GFP_H
#define _LINUXKPI_LINUX_GFP_H

#include <linux/types.h>

#define __GFP_ZERO        ((gfp_t)0x100u)
#define __GFP_NOWARN      ((gfp_t)0x200u)
#define __GFP_HIGH        ((gfp_t)0x20u)
#define __GFP_DMA         ((gfp_t)0x01u)
#define __GFP_DMA32       ((gfp_t)0x04u)
#define __GFP_HIGHMEM     ((gfp_t)0x02u)
#define __GFP_MOVABLE     ((gfp_t)0x08u)
#define __GFP_RECLAIM     ((gfp_t)0x10u)
#define __GFP_IO          ((gfp_t)0x40u)
#define __GFP_FS          ((gfp_t)0x80u)
#define __GFP_NORETRY     ((gfp_t)0x400u)
#define __GFP_COMP        ((gfp_t)0x800u)
#define __GFP_NOFAIL      ((gfp_t)0x1000u)
#define __GFP_RETRY_MAYFAIL ((gfp_t)0x2000u)

#define GFP_KERNEL        ((gfp_t)0u)
#define GFP_ATOMIC        ((gfp_t)0u)
#define GFP_NOWAIT        ((gfp_t)0u)
#define GFP_USER          ((gfp_t)0u)
#define GFP_DMA           __GFP_DMA
#define GFP_DMA32         __GFP_DMA32

#endif /* _LINUXKPI_LINUX_GFP_H */
#ifndef _LKPI_GFP_HIGHUSER
#define _LKPI_GFP_HIGHUSER
#define GFP_HIGHUSER  ((gfp_t)0u)
#define GFP_HIGHUSER_MOVABLE ((gfp_t)0u)
#define GFP_NOIO ((gfp_t)0u)
#define GFP_KERNEL_ACCOUNT ((gfp_t)0u)
#endif
