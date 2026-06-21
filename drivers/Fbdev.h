/*
 * Fbdev.h — the Linux framebuffer device (fbdev) interface for /dev/fb0.
 *
 * The structs match Linux <linux/fb.h> on i386, and the ioctl numbers are the real
 * FBIOGET_*SCREENINFO, so a program written for the Linux framebuffer (open /dev/fb0,
 * query the screeninfo, mmap, draw striding by line_length) runs unmodified. The fill
 * helpers + read/write are pure (host-testable); Fb0Device wraps them as a CharDevice.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct fb_bitfield {
	uint32_t offset;     // bit position of the field, LSB = 0
	uint32_t length;     // bit width
	uint32_t msb_right;  // != 0 if MSB is on the right
};

struct fb_fix_screeninfo {
	char     id[16];
	uint32_t smem_start;   // framebuffer physical address (unsigned long on i386 = 32-bit)
	uint32_t smem_len;     // framebuffer size in bytes
	uint32_t type;         // FB_TYPE_PACKED_PIXELS = 0
	uint32_t type_aux;
	uint32_t visual;       // FB_VISUAL_TRUECOLOR = 2
	uint16_t xpanstep, ypanstep, ywrapstep;
	uint32_t line_length;  // bytes per scanline (== pitch)
	uint32_t mmio_start, mmio_len, accel;
	uint16_t capabilities, reserved[2];
};

struct fb_var_screeninfo {
	uint32_t xres, yres;
	uint32_t xres_virtual, yres_virtual;
	uint32_t xoffset, yoffset;
	uint32_t bits_per_pixel;
	uint32_t grayscale;
	fb_bitfield red, green, blue, transp;
	uint32_t nonstd, activate;
	uint32_t height, width;     // physical dimensions in mm (-1 if unknown)
	uint32_t accel_flags;
	uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin,
	         hsync_len, vsync_len, sync, vmode, rotate, colorspace, reserved[4];
};

enum {
	FBIOGET_VSCREENINFO = 0x4600,
	FBIOGET_FSCREENINFO = 0x4602,
};

// A framebuffer description (the MI mirror of arch::BootFramebuffer).
struct FbInfo {
	uint64_t phys;             // physical base (identity-mapped in the kernel; 64-bit: real HW LFB >4 GiB)
	uint32_t pitch, width, height;
	uint8_t  bpp;
};

void fbdevFillFix(fb_fix_screeninfo* fix, const FbInfo& fb);
void fbdevFillVar(fb_var_screeninfo* var, const FbInfo& fb);
int  fbdevIoctl(unsigned cmd, void* arg, const FbInfo& fb);   // 0 ok, <0 -errno
// Copy to/from the framebuffer bytes, clamped to smem_len. Pure (lfb is the caller's).
int  fbdevRead(uint8_t* lfb, unsigned smemLen, unsigned off, void* buf, unsigned n);
int  fbdevWrite(uint8_t* lfb, unsigned smemLen, unsigned off, const void* buf, unsigned n);

}  // namespace kernel
