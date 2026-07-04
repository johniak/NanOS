/* linuxkpi/include/linux/fb.h — i915 fbdev-emulation state check. NanOS owns /dev/fb0 directly, so
 * the emulation is inert; i915_driver only reads the fbdev suspend state, which is always RUNNING. */
#ifndef _LKPI_LINUX_FB_H
#define _LKPI_LINUX_FB_H
#define FBINFO_STATE_RUNNING   0
#define FBINFO_STATE_SUSPENDED 1
struct fb_info { int state; void *par; };
#endif
