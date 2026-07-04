/* linuxkpi/include/acpi/video.h — ACPI backlight/video-switch registration. NanOS drives the panel
 * via i915 native backlight, so these register/unregister calls are inert. */
#ifndef _LKPI_ACPI_VIDEO_H
#define _LKPI_ACPI_VIDEO_H
static inline void acpi_video_register(void){ }
static inline void acpi_video_unregister(void){ }
static inline int acpi_video_get_backlight_type(void){ return 0; }
enum acpi_backlight_type { acpi_backlight_undef = -1, acpi_backlight_none = 0, acpi_backlight_video, acpi_backlight_vendor, acpi_backlight_native };
#endif
