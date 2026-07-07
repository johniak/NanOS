/*
 * i915_present.c — mirror the desktop onto the i915-driven panel.
 *
 * After the UNMODIFIED i915 probe modesets the Dell's eDP panel, the primary plane scans out the
 * stolen framebuffer i915 wrapped at DSM offset 0 (see the [drm] "Initial plane fb bound to 0x0 in
 * the ggtt" trail). fbcon + nwm, however, keep drawing to the bootloader's GOP framebuffer. If the
 * two ever differ, the panel is lit (backlight on, vblanks flowing) but shows the untouched stolen
 * buffer — this mirror bridges them, exactly like kext/virtio_gpu/virtio_gpu_present.c but with a
 * CPU memcpy instead of a virtio TRANSFER_TO_HOST_2D.
 *
 * THE DESTINATION MUST BE THE GTT APERTURE (GMADR/BAR2 + the plane's GGTT offset), NEVER the raw
 * stolen physical address. On Gen9 the DSM range is claimed by the system agent and CPU accesses
 * to it are architecturally disallowed (dropped or machine-hanging — boot #28 froze exactly here,
 * mid-memcpy into BDSM, with no exception and no further log lines). The aperture window is the
 * one CPU-legal view of stolen: GMADR reads/writes are translated through the GGTT, and i915 kept
 * the initial plane bound at GGTT 0 ("Initial plane fb bound to 0x0"), so GMADR+0 IS the panel.
 *
 * On this Dell the firmware GOP fb already sits at GMADR+0 (boot fb phys 0x80000000 == BAR2), so
 * fbcon/nwm writes land in the scanned-out stolen buffer through the aperture and no mirror is
 * needed at all — i915_entry.c detects that (boot fb == aperture target) and skips the arm. This
 * mirror only runs in the general case where the plane scans a different GGTT offset.
 */
#include <linux/types.h>
#include <linux/string.h>
#include "lkpi_knx.h"

/* Armed state, published to the present thread (single writer at bring-up, then read-only). */
static volatile int   g_armed;
static volatile int   g_suspended;  /* a userland KMS client owns the scanout (i915_drm_node.c) */
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

	if (!g_armed || g_suspended)
		return;
	for (y = 0; y < g_rows; y++)
		memcpy(g_dst + (unsigned long)y * g_dst_pitch,
		       g_src + (unsigned long)y * g_src_pitch, g_bpl);
}

/* Pause/resume the mirror while a userland KMS client (via /dev/dri, i915_drm_node.c) drives the
 * CRTC — an armed mirror would re-copy the boot fb over every frame the client presents. No-op
 * when the mirror was never armed (the self-map case). */
void i915_present_set_suspended(int s)
{
	g_suspended = s;
}

/* Arm the desktop->panel mirror after a successful i915 probe.
 *   scanout_phys  — APERTURE address of the plane buffer: GMADR (BAR2) + the plane's GGTT offset.
 *                   Never the raw stolen physical base — see the header comment.
 *   scanout_pitch — the plane stride in bytes, or 0 to inherit the boot fb's stride.
 * Source geometry is taken from the bootloader GOP fb (knx_boot_fb): i915 wrapped that same
 * firmware framebuffer, so their width/height match. Returns 0 on success, -1 if there is no boot
 * fb, no scanout base, or the aperture window cannot be mapped. Never faults — a bad arm just
 * leaves the panel showing the untouched plane buffer, same as before. */
int i915_present_bringup(unsigned long long scanout_phys, unsigned int scanout_pitch)
{
	unsigned long long src_phys = 0;
	unsigned int src_pitch = 0, src_w = 0, src_h = 0;
	unsigned char src_bpp = 0;
	unsigned int bpl, dst_pitch;

	if (!scanout_phys)
		return -1;
	if (!knx_boot_fb(&src_phys, &src_pitch, &src_w, &src_h, &src_bpp) || !src_phys)
		return -1;

	dst_pitch = scanout_pitch ? scanout_pitch : src_pitch;

	/* The aperture is a PCI BAR window, not RAM — map it explicitly (uncached is fine for a
	 * write-mostly mirror) instead of assuming the identity map covers it. */
	g_dst = (unsigned char *)knx_map_mmio(scanout_phys, (unsigned long long)src_h * dst_pitch);
	if (!g_dst)
		return -1;

	g_src       = (unsigned char *)(unsigned long)src_phys;
	g_src_pitch = src_pitch;
	g_dst_pitch = dst_pitch;

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
