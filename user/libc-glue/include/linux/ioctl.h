/*
 * linux/ioctl.h — the Linux-uapi spelling of the ioctl-number macros.
 *
 * Mesa's bundled drm-uapi headers (sync_file.h, pulled in by the iris fence code) include
 * <linux/ioctl.h> because the port's cc-wrap defines __linux__. All the actual _IOC layout
 * lives in <asm/ioctl.h> (which mainline linux/ioctl.h also just forwards to).
 */
#ifndef _NANOS_LINUX_IOCTL_H
#define _NANOS_LINUX_IOCTL_H
#include <asm/ioctl.h>
#endif
