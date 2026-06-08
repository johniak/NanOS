#include "Fbdev.h"
#include "string.h"

namespace kernel {

#define FB_EINVAL (-22)

void fbdevFillFix(fb_fix_screeninfo* f, const FbInfo& fb) {
	memset(f, 0, sizeof *f);
	const char* id = "nanosfb";
	for (int i = 0; id[i] && i < 15; i++)
		f->id[i] = id[i];
	f->smem_start = fb.phys;
	f->smem_len = fb.pitch * fb.height;
	f->type = 0;            // FB_TYPE_PACKED_PIXELS
	f->visual = 2;          // FB_VISUAL_TRUECOLOR
	f->line_length = fb.pitch;
}

void fbdevFillVar(fb_var_screeninfo* v, const FbInfo& fb) {
	memset(v, 0, sizeof *v);
	v->xres = v->xres_virtual = fb.width;
	v->yres = v->yres_virtual = fb.height;
	v->bits_per_pixel = fb.bpp;
	// 32bpp BGRX / 24bpp BGR: red at bit 16, green 8, blue 0 (each 8 bits).
	v->red.offset = 16;  v->red.length = 8;
	v->green.offset = 8; v->green.length = 8;
	v->blue.offset = 0;  v->blue.length = 8;
	if (fb.bpp == 32) { v->transp.offset = 24; v->transp.length = 8; }
	v->height = 0xFFFFFFFF;   // physical size unknown (-1)
	v->width = 0xFFFFFFFF;
}

int fbdevIoctl(unsigned cmd, void* arg, const FbInfo& fb) {
	if (!arg)
		return FB_EINVAL;
	if (cmd == FBIOGET_FSCREENINFO) {
		fbdevFillFix((fb_fix_screeninfo*) arg, fb);
		return 0;
	}
	if (cmd == FBIOGET_VSCREENINFO) {
		fbdevFillVar((fb_var_screeninfo*) arg, fb);
		return 0;
	}
	return FB_EINVAL;
}

int fbdevRead(uint8_t* lfb, unsigned smemLen, unsigned off, void* buf, unsigned n) {
	if (off >= smemLen)
		return 0;
	unsigned cnt = n < smemLen - off ? n : smemLen - off;
	memcpy(buf, lfb + off, cnt);
	return (int) cnt;
}

int fbdevWrite(uint8_t* lfb, unsigned smemLen, unsigned off, const void* buf, unsigned n) {
	if (off >= smemLen)
		return 0;
	unsigned cnt = n < smemLen - off ? n : smemLen - off;
	memcpy(lfb + off, buf, cnt);
	return (int) cnt;
}

}  // namespace kernel
