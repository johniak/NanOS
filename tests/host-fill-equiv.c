/*
 * host-fill-equiv.c — host-only equivalence + perf gate for the libnw glass fill fast paths.
 *
 * Compiles the REAL user/libnw/nw_gfx.c (with font/file stubs below) and checks that
 * nw_over_rect / nw_over_round / nw_over_round_soft produce BIT-IDENTICAL pixels to a naive
 * per-pixel reference (nw_over_pixel over every covered pixel, the pre-optimization shape),
 * across randomized rects, radii, feathers, scissors, ink colours and destination contents
 * (uniform glass canvases AND per-pixel random noise, to force the memo fast path to miss).
 *
 *   cc -O2 -I user/libnw -o /tmp/host-fill-equiv tests/host-fill-equiv.c && /tmp/host-fill-equiv
 *
 * Prints a small benchmark at the end (panel-sized fills) so a perf regression is visible.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nw_gfx.h"

/* ---- stubs so nw_gfx.c links without the TTF engine or the OS ---- */
#include "nwfont.h"
const unsigned char nx_font8x16[256][16];
int  nwfont_set(int role, const char *path, int px) { (void) role; (void) path; (void) px; return -1; }
int  nwfont_loaded(int role) { (void) role; return 0; }
const struct nwfont_glyph *nwfont_get(int role, unsigned cp) { (void) role; (void) cp; return 0; }
int  nwfont_ascent(int role) { (void) role; return 12; }
int  nwfont_text_w(int role, const char *s) { (void) role; (void) s; return 0; }

#include "nw_gfx.c"   /* the real rasterizer under test */

/* ---- naive references: the exact pre-optimization loops ---- */
static void ref_over_rect(const struct nw_surface *s, int x, int y, int w, int h, uint32_t argb)
{
	int bx0, by0, bx1, by1;
	nw_bounds(s, &bx0, &by0, &bx1, &by1);
	int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
	if (x0 < bx0) x0 = bx0;
	if (y0 < by0) y0 = by0;
	if (x1 > bx1) x1 = bx1;
	if (y1 > by1) y1 = by1;
	for (int yy = y0; yy < y1; yy++)
		for (int xx = x0; xx < x1; xx++)
			nw_over_pixel(s, xx, yy, argb);
}

static void ref_over_round_soft(const struct nw_surface *s, int x, int y, int w, int h, int r,
                                uint32_t argb, int feather)
{
	if (w <= 0 || h <= 0) return;
	if (r * 2 > w) r = w / 2;
	if (r * 2 > h) r = h / 2;
	if (feather < 1) { nw_over_round(s, x, y, w, h, r, argb); return; }
	unsigned sa = argb >> 24;
	uint32_t rgb = argb & 0x00ffffffu;
	int bx0, by0, bx1, by1;
	nw_bounds(s, &bx0, &by0, &bx1, &by1);
	int x0 = x < bx0 ? bx0 : x, y0 = y < by0 ? by0 : y;
	int x1 = x + w > bx1 ? bx1 : x + w, y1 = y + h > by1 ? by1 : y + h;
	float hw = w * 0.5f, hh = h * 0.5f;
	float cx = x + hw, cy = y + hh, fr = (float) r, ff = (float) feather;
	for (int yy = y0; yy < y1; yy++)
		for (int xx = x0; xx < x1; xx++) {
			float qx = xx + 0.5f - cx, qy = yy + 0.5f - cy;
			if (qx < 0) qx = -qx;
			if (qy < 0) qy = -qy;
			qx -= hw - fr;  qy -= hh - fr;
			float sd;
			if (qx > 0.0f && qy > 0.0f)
				sd = __builtin_sqrtf(qx * qx + qy * qy) - fr;
			else
				sd = (qx > qy ? qx : qy) - fr;
			float t = -sd / ff;
			if (t <= 0.0f) continue;
			if (t >= 1.0f) { nw_over_pixel(s, xx, yy, argb); continue; }
			t = t * t * (3.0f - 2.0f * t);
			unsigned a = (unsigned) ((float) sa * t + 0.5f);
			if (a) nw_over_pixel(s, xx, yy, (a << 24) | rgb);
		}
}

/* ---- randomized comparison harness ---- */
static unsigned rng_state = 0x1234567u;
static unsigned rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

#define W 400
#define H 300
static uint32_t bufa[W * H], bufb[W * H];

static int compare(const char *what, int it)
{
	for (long i = 0; i < (long) W * H; i++)
		if (bufa[i] != bufb[i]) {
			printf("FAIL %s iter %d: pixel %ld (%ld,%ld) got %08x want %08x\n",
			       what, it, i, i % W, i / W, bufa[i], bufb[i]);
			return 1;
		}
	return 0;
}

static void seed_dst(int noise)
{
	uint32_t base = 0x2a101014u;
	for (long i = 0; i < (long) W * H; i++) {
		uint32_t v = noise ? (rnd() & 0xffffffffu) : base;
		bufa[i] = bufb[i] = v;
	}
}

static double now_ms(void)
{
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(void)
{
	struct nw_surface sa = { bufa, W, H, W, 0, 0, 0, 0 };
	struct nw_surface sb = { bufb, W, H, W, 0, 0, 0, 0 };
	int fails = 0;

	for (int it = 0; it < 400; it++) {
		int noise = it & 1;
		seed_dst(noise);
		int x = (int) (rnd() % (W + 40)) - 20, y = (int) (rnd() % (H + 40)) - 20;
		int w = (int) (rnd() % (W + 20)), h = (int) (rnd() % (H + 20));
		int r = (int) (rnd() % 24), f = (int) (rnd() % 20);
		uint32_t ink = rnd();
		if ((it % 5) == 0) ink |= 0xff000000u;     /* exercise the opaque fast path */
		if ((it % 7) == 0) ink &= 0x00ffffffu;     /* and the sa==0 no-op */
		/* random scissor half the time (mirrors repaint_dirty's ancestor recompose) */
		if (it & 2) {
			int cx0 = (int) (rnd() % W), cy0 = (int) (rnd() % H);
			int cw = (int) (rnd() % W), ch = (int) (rnd() % H);
			sa.clip_x0 = sb.clip_x0 = cx0; sa.clip_y0 = sb.clip_y0 = cy0;
			sa.clip_x1 = sb.clip_x1 = cx0 + cw; sa.clip_y1 = sb.clip_y1 = cy0 + ch;
		} else {
			sa.clip_x0 = sb.clip_x0 = sa.clip_y0 = sb.clip_y0 = 0;
			sa.clip_x1 = sb.clip_x1 = sa.clip_y1 = sb.clip_y1 = 0;
		}

		switch (it % 3) {
		case 0:
			nw_over_rect(&sa, x, y, w, h, ink);
			ref_over_rect(&sb, x, y, w, h, ink);
			fails += compare("over_rect", it);
			break;
		case 1:
			nw_over_round_soft(&sa, x, y, w, h, r, ink, f);
			ref_over_round_soft(&sb, x, y, w, h, r, ink, f);
			fails += compare("over_round_soft", it);
			break;
		case 2:
			/* nw_over_round routes its body through nw_over_rect: compare against a
			 * reference round built on the naive rect */
			nw_over_round(&sa, x, y, w, h, r, ink);
			{	/* reference: same corner code (shared), naive strips */
				if (r * 2 > w) r = w / 2;
				if (r * 2 > h) r = h / 2;
				if (r < 1) {
					ref_over_rect(&sb, x, y, w, h, ink);
				} else {
					unsigned ia = ink >> 24;
					uint32_t rgb = ink & 0x00ffffffu;
					ref_over_rect(&sb, x, y + r, w, h - 2 * r, ink);
					ref_over_rect(&sb, x + r, y, w - 2 * r, r, ink);
					ref_over_rect(&sb, x + r, y + h - r, w - 2 * r, r, ink);
					int cxs[4] = { x + r, x + w - r, x + r, x + w - r };
					int cys[4] = { y + r, y + r, y + h - r, y + h - r };
					for (int k = 0; k < 4; k++) {
						int ox = (k & 1) ? cxs[k] : cxs[k] - r;
						int oy = (k & 2) ? cys[k] : cys[k] - r;
						for (int yy = 0; yy < r; yy++)
							for (int xx = 0; xx < r; xx++) {
								float dx = (ox + xx) + 0.5f - cxs[k];
								float dy = (oy + yy) + 0.5f - cys[k];
								int cov = edge_cov(__builtin_sqrtf(dx * dx + dy * dy), r);
								if (cov) {
									unsigned a2 = (unsigned) (cov * (int) ia) / 255;
									nw_over_pixel(&sb, ox + xx, oy + yy, (a2 << 24) | rgb);
								}
							}
					}
				}
			}
			fails += compare("over_round", it);
			break;
		}
	}

	if (fails) { printf("host-fill-equiv: %d FAILURES\n", fails); return 1; }
	printf("host-fill-equiv OK (400 randomized cases, bit-identical)\n");

	/* ---- perf snapshot: maximized-window panel fill ---- */
	{
		int reps = 40;
		static uint32_t big[1280 * 800];
		struct nw_surface s = { big, 1280, 800, 1280, 0, 0, 0, 0 };
		for (long i = 0; i < 1280L * 800; i++) big[i] = 0x2a101014u;
		double t0 = now_ms();
		for (int i = 0; i < reps; i++) nw_over_rect(&s, 32, 40, 1216, 720, 0x59202028u);
		double t1 = now_ms();
		printf("perf: over_rect 1216x720      %7.3f ms/op\n", (t1 - t0) / reps);
		t0 = now_ms();
		for (int i = 0; i < reps; i++) nw_over_round_soft(&s, 32, 40, 1216, 720, 14, 0x59202028u, 14);
		t1 = now_ms();
		printf("perf: over_round_soft 1216x720 %6.3f ms/op\n", (t1 - t0) / reps);
	}
	return 0;
}
