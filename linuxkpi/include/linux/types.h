/*
 * linuxkpi/include/linux/types.h — minimal Linux kernel <linux/types.h> for the NanOS
 * LinuxKPI shim. Provides the fixed-width and endian-tagged integer types Linux driver
 * source expects. Backed by the freestanding <stdint.h>/<stddef.h> (available even under
 * -ffreestanding), so it works in both the kext build and host doctest builds.
 */
#ifndef _LINUXKPI_LINUX_TYPES_H
#define _LINUXKPI_LINUX_TYPES_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t   __s8;
typedef int16_t  __s16;
typedef int32_t  __s32;
typedef int64_t  __s64;

/* Endian-tagged aliases (no byteswap semantics enforced here — virtio uses helpers). */
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint16_t __be16;
typedef uint32_t __be32;
typedef uint64_t __be64;
typedef uint16_t __virtio16;
typedef uint32_t __virtio32;
typedef uint64_t __virtio64;

typedef u64 dma_addr_t;
typedef u64 phys_addr_t;
typedef u64 resource_size_t;
typedef s64 loff_t;
typedef u32 gfp_t;
typedef int bool_unused_; /* placeholder to keep section non-empty if <stdbool.h> absent */

#ifndef __cplusplus
#include <stdbool.h>
#endif

#endif /* _LINUXKPI_LINUX_TYPES_H */
