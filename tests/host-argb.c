/* cc -o /tmp/host-argb tests/host-argb.c && /tmp/host-argb  (host-only helper test) */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
struct nw_surface { uint32_t *px; int w, h, stride; int clip_x0, clip_y0, clip_x1, clip_y1; };
#define NW_ARGB_TEST_HOST 1
#include "../user/libnw/nw_over_core.h"   /* see note below */
int main(void)
{
	uint32_t buf[4] = {0, 0, 0, 0};
	struct nw_surface s = { buf, 4, 1, 4, 0, 0, 3, 0 };
	nw_over_pixel(&s, 0, 0, 0xff112233u);            assert(buf[0] == 0xff112233u); /* opaque copy */
	nw_over_pixel(&s, 1, 0, 0x80ffffffu);            /* 50% white over transparent */
	assert((buf[1] >> 24) == 0x80 && ((buf[1] >> 16) & 0xff) == 0xff);
	nw_over_pixel(&s, 1, 0, 0x80000000u | 0x000000u);/* 50% black over that */
	unsigned a = buf[1] >> 24;                        /* 0.5 + 0.5*0.5 = 0.75 */
	assert(a >= 0xbe && a <= 0xc2);
	unsigned r = (buf[1] >> 16) & 0xff;               /* (0*128 + 255*64)/191 ~ 85 */
	assert(r >= 82 && r <= 88);
	nw_over_pixel(&s, 2, 0, 0x00123456u);            assert(buf[2] == 0);          /* a=0 no-op */
	printf("host-argb OK\n");
	return 0;
}
