/*
 * fbtest — draw a gradient into /dev/fb0 the Linux way: open the framebuffer, query
 * the screeninfo with the fbdev ioctls, mmap it, and write pixels striding by the
 * reported line_length. This is the "reuse Linux framebuffer software" proof.
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

/* libc glue (user/libc-glue/syscalls.c). */
extern int ioctl(int fd, unsigned long request, ...);
extern void* mmap(void* addr, unsigned long length, int prot, int flags, int fd, long off);

/* The Linux fbdev structs we use (subset; layout matches <linux/fb.h> on i386). */
struct fb_bitfield { uint32_t offset, length, msb_right; };
struct fb_fix_screeninfo {
	char id[16];
	uint32_t smem_start, smem_len, type, type_aux, visual;
	uint16_t xpanstep, ypanstep, ywrapstep;
	uint32_t line_length, mmio_start, mmio_len, accel;
	uint16_t capabilities, reserved[2];
};
struct fb_var_screeninfo {
	uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
	uint32_t bits_per_pixel, grayscale;
	struct fb_bitfield red, green, blue, transp;
	uint32_t nonstd, activate, height, width, accel_flags;
	uint32_t pixclock, lm, rm, um, lmar, hslen, vslen, sync, vmode, rotate, cs, resv[4];
};
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

int main(void) {
	int fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		printf("fbtest: cannot open /dev/fb0\n");
		return 1;
	}
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	ioctl(fd, FBIOGET_VSCREENINFO, &var);
	ioctl(fd, FBIOGET_FSCREENINFO, &fix);
	printf("fbtest: %ux%u bpp%u pitch %u\n",
			var.xres, var.yres, var.bits_per_pixel, fix.line_length);

	unsigned char* fb = (unsigned char*) mmap(0, fix.smem_len, 3 /*RW*/, 1 /*SHARED*/, fd, 0);
	if (fb == (void*) -1) {
		printf("fbtest: mmap failed\n");
		return 1;
	}

	for (uint32_t y = 0; y < var.yres; y++) {
		uint32_t* row = (uint32_t*) (fb + y * fix.line_length);
		for (uint32_t x = 0; x < var.xres; x++) {
			uint32_t r = x * 255 / var.xres;
			uint32_t g = y * 255 / var.yres;
			row[x] = (r << 16) | (g << 8);          /* red across, green down */
		}
	}
	printf("fbtest: drew gradient via mmap\n");
	return 0;
}
