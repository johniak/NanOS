/*
 * i915_present.c — mirror the desktop onto the i915-driven panel.
 *
 * After the UNMODIFIED i915 probe modesets the Dell's eDP panel, the primary plane scans out the
 * stolen framebuffer i915 wrapped at DSM offset 0 (see the [drm] "Initial plane fb bound to 0x0 in
 * the ggtt" trail). fbcon + nwm, however, keep drawing to the bootloader's GOP framebuffer — the
 * buffer the firmware used to scan out, now ORPHANED once i915 owns the panel. The panel is lit
 * (backlight on, vblanks flowing) but shows the untouched stolen buffer: a black screen.
 *
 * This bridges the two exactly like kext/virtio_gpu/virtio_gpu_present.c, except the "present" is a
 * CPU memcpy instead of a virtio TRANSFER_TO_HOST_2D: the periodic present thread copies what the
 * desktop drew (the GOP fb) into what the panel shows (the i915 scanout) every frame. Nothing
 * reassigns /dev/fb0, so the console VT surface and nwm keep working against the boot fb unchanged
 * — the mirror is a pure read-of-src / write-of-dst overlay. Both buffers are identity-mapped
 * physical memory (NanOS maps all RAM 1:1), so a physical address IS the CPU address.
 *
 * If the GOP fb and the i915 scanout ever resolve to the same physical buffer, the copy is a
 * harmless self-copy; if they differ (the observed Dell case), it makes the desktop visible.
 */
#include <linux/types.h>
#include <linux/string.h>
#include "lkpi_knx.h"

/* Armed state, published to the present thread (single writer at bring-up, then read-only). */
static volatile int   g_armed;
static unsigned char *g_src;        /* GOP boot fb — what fbcon/nwm draw into            */
static unsigned char *g_dst;        /* i915 scanout — what the eDP panel scans out       */
static unsigned int   g_src_pitch;  /* source stride (bytes/row)                          */
static unsigned int   g_dst_pitch;  /* scanout stride (bytes/row)                         */
static unsigned int   g_bpl;        /* bytes copied per row = min(src bpl, both pitches)  */
static unsigned int   g_rows;       /* rows copied = min(src h, dst h)                     */

/* Periodic present (runs on the kernel present thread at ~30 fps, post-scheduler). Copies the boot
 * framebuffer row-by-row into the i915 scanout, honouring each buffer's own stride so a padded
 * pitch shears nothing. */
static void i915_mirror_flush(void)
{
	unsigned int y;

	if (!g_armed)
		return;
	for (y = 0; y < g_rows; y++)
		memcpy(g_dst + (unsigned long)y * g_dst_pitch,
		       g_src + (unsigned long)y * g_src_pitch, g_bpl);
}

/* Arm the desktop->panel mirror after a successful i915 probe.
 *   scanout_phys  — physical base of the i915 primary-plane buffer (stolen base + plane offset 0).
 *   scanout_pitch — the plane stride in bytes, or 0 to inherit the boot fb's stride.
 * Source geometry is taken from the bootloader GOP fb (knx_boot_fb): i915 wrapped that same
 * firmware framebuffer, so their width/height match. Returns 0 on success, -1 if there is no boot
 * fb or the scanout base is zero (stolen disabled). Never faults — a bad arm just leaves the panel
 * black, same as before. */
int i915_present_bringup(unsigned long long scanout_phys, unsigned int scanout_pitch)
{
	unsigned long long src_phys = 0;
	unsigned int src_pitch = 0, src_w = 0, src_h = 0;
	unsigned char src_bpp = 0;
	unsigned int bpl;

	if (!scanout_phys)
		return -1;
	if (!knx_boot_fb(&src_phys, &src_pitch, &src_w, &src_h, &src_bpp) || !src_phys)
		return -1;

	g_src       = (unsigned char *)(unsigned long)src_phys;
	g_dst       = (unsigned char *)(unsigned long)scanout_phys;
	g_src_pitch = src_pitch;
	g_dst_pitch = scanout_pitch ? scanout_pitch : src_pitch;

	/* One row is width*4 bytes (32bpp); never copy past either stride. */
	bpl = src_w * 4u;
	if (bpl > g_src_pitch)
		bpl = g_src_pitch;
	if (bpl > g_dst_pitch)
		bpl = g_dst_pitch;
	g_bpl  = bpl;
	g_rows = src_h;

	g_armed = 1;
	i915_mirror_flush();                       /* present the current desktop frame immediately  */
	knx_fb_start_present(&i915_mirror_flush);  /* then ~30 fps via the kernel present thread      */
	return 0;
}
