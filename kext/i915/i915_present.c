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

/* ---- scanout snapshot/restore: bring the console back after the last KMS client exits ------ *
 *
 * When a /dev/dri client closes, DRM core removes its framebuffers; atomic_remove_fb
 * (drm_framebuffer.c:969) DISABLES the primary plane (the CRTC/transcoder stays active — full
 * CRTC teardown is only its -EINVAL retry path, and i915 accepts a planeless active CRTC). The
 * plane-disable commit zeroes PLANE_CTL/PLANE_SURF *and* the plane's watermarks + DDB slice, so
 * just setting the enable bit back would scan with zero FIFO allocation (underruns). Instead:
 * snapshot the full live plane 1A register set right after probe (boot fb on screen), and replay
 * it when the last client goes away. Register-level glue behind i915's back, same documented
 * class as the FBC disable in i915_entry.c: nothing commits after probe at this bring-up stage
 * except our clients, and the next client SETCRTC reprograms everything anyway.
 *
 * Offsets (display ver 9, pipe A plane 1 — skl_universal_plane_regs.h / skl_watermark_regs.h):
 *   PLANE_CTL 0x70180, STRIDE 0x70188, POS 0x7018c, SIZE 0x70190, SURF 0x7019c, OFFSET 0x701a4,
 *   PLANE_WM_1_A_0..7 0x70240+4*n, PLANE_WM_TRANS 0x70268, PLANE_BUF_CFG 0x7027c.
 * Guard: TRANS_CONF for transcoder EDP = 0x7f008 (pipe_offsets[TRANSCODER_EDP]=0x7f000 +
 * _TRANSACONF's 0x8) bit31 — if the transcoder is off, a plane replay cannot help (full modeset
 * needed); skip and say so. PLANE_SURF is written LAST: it arms the double-buffered update. */
static volatile unsigned int *g_mmio;
static unsigned int g_snap_geo[6];   /* CTL, STRIDE, POS, SIZE, OFFSET, SURF */
static unsigned int g_snap_wm[10];   /* WM0..7, WM_TRANS, BUF_CFG */
static int g_have_snap;

void i915_scanout_snapshot(volatile unsigned int *mmio)
{
	int i;
	if (!mmio)
		return;
	g_mmio = mmio;
	g_snap_geo[0] = mmio[0x70180 / 4];   /* PLANE_CTL_1_A    */
	g_snap_geo[1] = mmio[0x70188 / 4];   /* PLANE_STRIDE_1_A */
	g_snap_geo[2] = mmio[0x7018c / 4];   /* PLANE_POS_1_A    */
	g_snap_geo[3] = mmio[0x70190 / 4];   /* PLANE_SIZE_1_A   */
	g_snap_geo[4] = mmio[0x701a4 / 4];   /* PLANE_OFFSET_1_A */
	g_snap_geo[5] = mmio[0x7019c / 4];   /* PLANE_SURF_1_A   */
	for (i = 0; i < 8; i++)
		g_snap_wm[i] = mmio[(0x70240 + 4 * i) / 4];   /* PLANE_WM_1_A_0..7 */
	g_snap_wm[8] = mmio[0x70268 / 4];    /* PLANE_WM_TRANS_1_A */
	g_snap_wm[9] = mmio[0x7027c / 4];    /* PLANE_BUF_CFG_1_A  */
	g_have_snap = 1;
}

int i915_scanout_restore(void)
{
	int i;
	if (!g_have_snap)
		return -1;
	if (!(g_mmio[0x7f008 / 4] & 0x80000000u))   /* TRANS_CONF (EDP) enable */
		return -2;
	for (i = 0; i < 8; i++)
		g_mmio[(0x70240 + 4 * i) / 4] = g_snap_wm[i];
	g_mmio[0x70268 / 4] = g_snap_wm[8];
	g_mmio[0x7027c / 4] = g_snap_wm[9];
	g_mmio[0x70180 / 4] = g_snap_geo[0];
	g_mmio[0x70188 / 4] = g_snap_geo[1];
	g_mmio[0x7018c / 4] = g_snap_geo[2];
	g_mmio[0x70190 / 4] = g_snap_geo[3];
	g_mmio[0x701a4 / 4] = g_snap_geo[4];
	g_mmio[0x7019c / 4] = g_snap_geo[5];   /* SURF last — arms the update */
	return 0;
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
