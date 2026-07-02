/* linux/types.h — kernel-style fixed-width types for the DRM/virtgpu uapi headers when they take
 * their __linux__ branch (Mesa builds with -D__linux__ since NanOS is Linux-ABI). Sourced from
 * stdint; __user/__kernel annotations are no-ops in userspace. */
#ifndef _NANOS_LINUX_TYPES_H
#define _NANOS_LINUX_TYPES_H
#include <stdint.h>
typedef uint8_t  __u8;   typedef int8_t  __s8;
typedef uint16_t __u16;  typedef int16_t __s16;
typedef uint32_t __u32;  typedef int32_t __s32;
typedef uint64_t __u64;  typedef int64_t __s64;
typedef __u16 __le16; typedef __u16 __be16;
typedef __u32 __le32; typedef __u32 __be32;
typedef __u64 __le64; typedef __u64 __be64;
typedef unsigned long __kernel_size_t; typedef long __kernel_ssize_t; typedef long __kernel_ptrdiff_t;
typedef int __kernel_pid_t;
typedef __u16 __sum16; typedef __u32 __wsum;
#ifndef __user
#define __user
#endif
#ifndef __kernel
#define __kernel
#endif
#endif
