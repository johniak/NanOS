#include "doctest.h"
#include "Fbdev.h"
#include <cstring>

using namespace kernel;

TEST_CASE("fbdevFillFix: line_length=pitch, smem_len=pitch*height, truecolor") {
	FbInfo fb{ 0xFD000000, 4096, 1024, 768, 32 };
	fb_fix_screeninfo f;
	fbdevFillFix(&f, fb);
	CHECK(f.smem_start == 0xFD000000u);
	CHECK(f.line_length == 4096u);
	CHECK(f.smem_len == 4096u * 768u);
	CHECK(f.type == 0);       // FB_TYPE_PACKED_PIXELS
	CHECK(f.visual == 2);     // FB_VISUAL_TRUECOLOR
}

TEST_CASE("fbdevFillVar: resolution, bpp, and 8/8/8 RGB bitfields") {
	FbInfo fb{ 0xFD000000, 4096, 1024, 768, 32 };
	fb_var_screeninfo v;
	fbdevFillVar(&v, fb);
	CHECK(v.xres == 1024u);
	CHECK(v.yres == 768u);
	CHECK(v.xres_virtual == 1024u);
	CHECK(v.yres_virtual == 768u);
	CHECK(v.bits_per_pixel == 32u);
	CHECK(v.red.offset == 16);   CHECK(v.red.length == 8);
	CHECK(v.green.offset == 8);  CHECK(v.green.length == 8);
	CHECK(v.blue.offset == 0);   CHECK(v.blue.length == 8);
	CHECK(v.transp.length == 8); // 32bpp carries an alpha/X channel
}

TEST_CASE("fbdevFillVar: 24bpp has no transparency channel") {
	FbInfo fb{ 0xFD000000, 3072, 1024, 768, 24 };
	fb_var_screeninfo v;
	fbdevFillVar(&v, fb);
	CHECK(v.bits_per_pixel == 24u);
	CHECK(v.transp.length == 0u);
}

TEST_CASE("fbdevIoctl: GET cmds fill the structs; unknown/null -> -EINVAL") {
	FbInfo fb{ 0xFD000000, 4096, 1024, 768, 32 };
	fb_fix_screeninfo f;
	fb_var_screeninfo v;
	CHECK(fbdevIoctl(FBIOGET_FSCREENINFO, &f, fb) == 0);
	CHECK(f.line_length == 4096u);
	CHECK(fbdevIoctl(FBIOGET_VSCREENINFO, &v, fb) == 0);
	CHECK(v.xres == 1024u);
	CHECK(fbdevIoctl(0x1234, &v, fb) == -22);
	CHECK(fbdevIoctl(FBIOGET_FSCREENINFO, nullptr, fb) == -22);
}

TEST_CASE("fbdevRead/Write: clamp to smem_len and honor the offset") {
	unsigned char lfb[64];
	memset(lfb, 0, sizeof lfb);
	unsigned char src[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	CHECK(fbdevWrite(lfb, 64, 10, src, 8) == 8);
	CHECK(lfb[10] == 1);
	CHECK(lfb[17] == 8);
	CHECK(fbdevWrite(lfb, 64, 60, src, 8) == 4);   // only 4 bytes fit before smem_len

	unsigned char dst[8] = { 0 };
	CHECK(fbdevRead(lfb, 64, 10, dst, 8) == 8);
	CHECK(dst[0] == 1);
	CHECK(dst[7] == 8);
	CHECK(fbdevRead(lfb, 64, 64, dst, 8) == 0);    // offset past the end -> EOF
}
