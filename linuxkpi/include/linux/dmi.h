/*
 * linuxkpi/include/linux/dmi.h — minimal DMI shim.
 *
 * NanOS has no DMI/SMBIOS table parsing, so DMI matching always misses. This is only
 * pulled in by drm_panel_orientation_quirks.c, whose quirk table keys on laptop DMI
 * strings — none of which apply to a virtio-gpu guest, so "no match" is the correct
 * behaviour (the driver falls back to DRM_MODE_PANEL_ORIENTATION_UNKNOWN).
 */
#ifndef _LKPI_DMI_H
#define _LKPI_DMI_H
#include <linux/types.h>

enum dmi_field {
	DMI_NONE, DMI_BIOS_VENDOR, DMI_BIOS_VERSION, DMI_BIOS_DATE, DMI_BIOS_RELEASE,
	DMI_SYS_VENDOR, DMI_PRODUCT_NAME, DMI_PRODUCT_VERSION, DMI_PRODUCT_SERIAL,
	DMI_PRODUCT_UUID, DMI_PRODUCT_SKU, DMI_PRODUCT_FAMILY, DMI_BOARD_VENDOR,
	DMI_BOARD_NAME, DMI_BOARD_VERSION, DMI_BOARD_SERIAL, DMI_BOARD_ASSET_TAG,
	DMI_CHASSIS_VENDOR, DMI_CHASSIS_TYPE, DMI_CHASSIS_VERSION, DMI_CHASSIS_SERIAL,
	DMI_CHASSIS_ASSET_TAG, DMI_STRING_MAX, DMI_OEM_STRING,
};

struct dmi_strmatch { unsigned char slot; char substr[79]; };
struct dmi_system_id {
	int (*callback)(const struct dmi_system_id *);
	const char *ident;
	struct dmi_strmatch matches[4];
	void *driver_data;
};
#define DMI_MATCH(a, b)  { .slot = a, .substr = b }
#define DMI_EXACT_MATCH(a, b) { .slot = a, .substr = b }

static inline int dmi_check_system(const struct dmi_system_id *list){ (void)list; return 0; }
static inline const struct dmi_system_id *dmi_first_match(const struct dmi_system_id *list){ (void)list; return 0; }
static inline const char *dmi_get_system_info(int field){ (void)field; return 0; }
static inline bool dmi_match(int f, const char *str){ (void)f; (void)str; return false; }

#endif /* _LKPI_DMI_H */
