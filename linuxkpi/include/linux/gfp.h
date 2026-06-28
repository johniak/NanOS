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

#define GFP_KERNEL        ((gfp_t)0u)
#define GFP_ATOMIC        ((gfp_t)0u)
#define GFP_NOWAIT        ((gfp_t)0u)
#define GFP_USER          ((gfp_t)0u)
#define GFP_DMA           __GFP_DMA
#define GFP_DMA32         __GFP_DMA32

#endif /* _LINUXKPI_LINUX_GFP_H */
