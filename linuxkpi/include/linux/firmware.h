/*
 * linuxkpi/include/linux/firmware.h — request_firmware over the NanOS VFS (kpi_firmware.c).
 *
 * The driver asks for a blob by name; we read /disks/main/nanos/firmware/<name> through the
 * kernel VFS (knx_file_read). Absent blobs return -ENOENT, which Gen9 i915 tolerates for the
 * optional GuC/HuC/DMC images (it runs execlists + no display-C-state firmware on that gen).
 */
#ifndef _LINUXKPI_LINUX_FIRMWARE_H
#define _LINUXKPI_LINUX_FIRMWARE_H

#include <linux/types.h>   /* size_t, u8, gfp_t */

#ifdef __cplusplus
extern "C" {
#endif

struct device;
struct module;

struct firmware {
	size_t size;
	const u8 *data;
	void *priv;          /* shim: the heap allocation backing `data` (release_firmware frees it) */
};

int  request_firmware(const struct firmware **fw, const char *name, struct device *device);
int  firmware_request_nowarn(const struct firmware **fw, const char *name, struct device *device);
int  request_firmware_direct(const struct firmware **fw, const char *name, struct device *device);
void release_firmware(const struct firmware *fw);

/* Async variant: NanOS bring-up is cooperative, so this loads synchronously and calls cont()
 * inline (uevent is ignored). Returns 0 (the request was issued); cont sees NULL fw on failure. */
int request_firmware_nowait(struct module *module, bool uevent, const char *name,
		struct device *device, gfp_t gfp, void *context,
		void (*cont)(const struct firmware *fw, void *context));

#ifdef __cplusplus
}
#endif

#endif /* _LINUXKPI_LINUX_FIRMWARE_H */
