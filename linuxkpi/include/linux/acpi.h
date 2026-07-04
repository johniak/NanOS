/* linuxkpi/include/linux/acpi.h — minimal ACPI surface. NanOS has no ACPICA, so handles are opaque
 * and lookups fail; i915 DSI/VBT code that walks ACPI objects degrades to its no-ACPI path. */
#ifndef _LKPI_LINUX_ACPI_H
#define _LKPI_LINUX_ACPI_H
#include <linux/types.h>

typedef void *acpi_handle;
typedef u32 acpi_status;
typedef u64 acpi_physical_address;
#define AE_OK 0
#define ACPI_HANDLE(dev) ((acpi_handle)0)
#define ACPI_SUCCESS(s)  ((s) == AE_OK)
#define ACPI_FAILURE(s)  ((s) != AE_OK)

struct acpi_device;
struct acpi_buffer { unsigned long length; void *pointer; };
static inline struct acpi_device *acpi_dev_get_first_match_dev(const char *hid, const char *uid, int hrv){ (void)hid;(void)uid;(void)hrv; return 0; }
static inline acpi_handle acpi_device_handle(struct acpi_device *d){ (void)d; return 0; }
static inline bool acpi_dev_present(const char *hid, const char *uid, int hrv){ (void)hid;(void)uid;(void)hrv; return false; }

#endif /* _LKPI_LINUX_ACPI_H */
