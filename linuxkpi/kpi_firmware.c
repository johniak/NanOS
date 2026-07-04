/*
 * linuxkpi/kpi_firmware.c — request_firmware over the NanOS VFS.
 *
 * A driver names a blob; we read /disks/main/nanos/firmware/<name> through the kernel VFS
 * (knx_file_read). Missing blobs return -ENOENT, which Gen9 i915 tolerates for the optional
 * GuC/HuC/DMC firmware. The blob is copied into a heap allocation owned by `struct firmware`;
 * release_firmware frees both the buffer and the descriptor.
 */
#include <linux/firmware.h>
#include <linux/errno.h>
#include "lkpi_knx.h"

/* Runtime location of firmware blobs (mirrors the /nanos/kext load path in Kernel.cpp). */
#define FW_DIR     "/disks/main/nanos/firmware/"
#define FW_PATHMAX 192

/* Build FW_DIR + name into `out` (capacity FW_PATHMAX). Returns 0, or -1 if it would overflow. */
static int fw_build_path(char *out, const char *name)
{
	int i = 0;
	const char *p = FW_DIR;
	while (*p) {
		if (i >= FW_PATHMAX - 1) return -1;
		out[i++] = *p++;
	}
	while (*name) {
		if (i >= FW_PATHMAX - 1) return -1;
		out[i++] = *name++;
	}
	out[i] = 0;
	return 0;
}

static int fw_load(const struct firmware **out, const char *name)
{
	char path[FW_PATHMAX];
	unsigned long size = 0, got = 0;
	struct firmware *fw;
	void *buf;

	if (out) *out = 0;
	if (!out || !name) return -EINVAL;
	if (fw_build_path(path, name) < 0) return -ENOENT;

	/* size probe: knx_file_read(buf==0) reports the size (or fails if absent). */
	if (knx_file_read(path, 0, 0, &size) < 0) return -ENOENT;

	fw = (struct firmware *) knx_malloc(sizeof *fw);
	if (!fw) return -ENOMEM;
	fw->size = 0;
	fw->data = 0;
	fw->priv = 0;

	if (size) {
		buf = knx_malloc((unsigned) size);
		if (!buf) { knx_free(fw); return -ENOMEM; }
		if (knx_file_read(path, buf, size, &got) < 0) {
			knx_free(buf);
			knx_free(fw);
			return -ENOENT;
		}
		fw->data = (const u8 *) buf;
		fw->size = (size_t) got;
		fw->priv = buf;
	}
	*out = fw;
	return 0;
}

int request_firmware(const struct firmware **fw, const char *name, struct device *device)
{
	(void) device;
	return fw_load(fw, name);
}

int firmware_request_nowarn(const struct firmware **fw, const char *name, struct device *device)
{
	(void) device;
	return fw_load(fw, name);   /* the "nowarn" difference is only log verbosity, which we omit */
}

int request_firmware_direct(const struct firmware **fw, const char *name, struct device *device)
{
	(void) device;
	return fw_load(fw, name);   /* no userspace fallback helper exists here; direct == normal */
}

void release_firmware(const struct firmware *fw)
{
	if (!fw) return;
	if (fw->priv) knx_free(fw->priv);
	knx_free((void *) fw);
}

int request_firmware_nowait(struct module *module, bool uevent, const char *name,
		struct device *device, gfp_t gfp, void *context,
		void (*cont)(const struct firmware *fw, void *context))
{
	const struct firmware *fw = 0;
	(void) module; (void) uevent; (void) device; (void) gfp;
	/* Cooperative bring-up: load synchronously and hand the result to the callback inline.
	 * cont() sees a NULL fw on failure, exactly as the async helper would on -ENOENT. */
	fw_load(&fw, name);
	if (cont) cont(fw, context);
	return 0;
}
