/*
 * sys/ioccom.h — drmtest's shim for the vendored drm.h BSD branch.
 *
 * With neither __KERNEL__ nor __linux__ defined (picolibc userspace), <drm/drm.h> takes its "one
 * of the BSDs" branch: it self-defines the __uNN types but reaches for <sys/ioccom.h> to get the
 * ioctl-number encoding. We must use the LINUX _IOC layout here (not the BSD one), because the
 * ioctl numbers have to match what the kernel-side DRM core decodes — that is exactly the same
 * <asm/ioctl.h> the kext compiles against, so include it directly (no drift). Also define __user
 * (empty), which the BSD branch does not.
 */
#pragma once
#include <asm/ioctl.h>   /* linuxkpi/include/asm/ioctl.h — identical _IOC to the kernel side */
#ifndef __user
#define __user
#endif
