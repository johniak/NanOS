#include "nw_backdrop.h"

int nw_rect_empty(nw_rect r) { return r.w <= 0 || r.h <= 0; }

int nw_rect_intersects(nw_rect a, nw_rect b)
{
	if (nw_rect_empty(a) || nw_rect_empty(b)) return 0;
	return a.x < b.x + b.w && b.x < a.x + a.w &&
	       a.y < b.y + b.h && b.y < a.y + a.h;
}

nw_rect nw_rect_expand(nw_rect r, int by)
{
	nw_rect o = { r.x - by, r.y - by, r.w + 2 * by, r.h + 2 * by };
	if (o.w < 0) o.w = 0;
	if (o.h < 0) o.h = 0;
	return o;
}

nw_rect nw_rect_clamp(nw_rect r, int sw, int sh)
{
	int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
	int x1 = r.x + r.w, y1 = r.y + r.h;
	if (x1 > sw) x1 = sw;
	if (y1 > sh) y1 = sh;
	nw_rect o = { x0, y0, x1 - x0, y1 - y0 };
	if (o.w < 0) o.w = 0;
	if (o.h < 0) o.h = 0;
	return o;
}

nw_rect nw_rect_intersect(nw_rect a, nw_rect b)
{
	int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
	int ax1 = a.x + a.w, ay1 = a.y + a.h, bx1 = b.x + b.w, by1 = b.y + b.h;
	int x1 = ax1 < bx1 ? ax1 : bx1, y1 = ay1 < by1 ? ay1 : by1;
	nw_rect o = { x0, y0, x1 - x0, y1 - y0 };
	if (o.w < 0) o.w = 0;
	if (o.h < 0) o.h = 0;
	return o;
}

nw_rect nw_rect_union(nw_rect a, nw_rect b)
{
	if (nw_rect_empty(a)) return b;
	if (nw_rect_empty(b)) return a;
	int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
	int ax1 = a.x + a.w, ay1 = a.y + a.h, bx1 = b.x + b.w, by1 = b.y + b.h;
	int x1 = ax1 > bx1 ? ax1 : bx1, y1 = ay1 > by1 ? ay1 : by1;
	nw_rect o = { x0, y0, x1 - x0, y1 - y0 };
	return o;
}

nw_rect nw_cache_rect(nw_rect win, int blur_radius, int sw, int sh)
{
	return nw_rect_clamp(nw_rect_expand(win, blur_radius), sw, sh);
}
