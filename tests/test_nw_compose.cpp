#include "doctest.h"
#include "nw_compose.h"
#include "nwm_core.h"
#include "nwproto.h"
#include <vector>
#include <cstring>

// The opaque/square path (scratch=NULL, wall=NULL) is what these tests exercise: nw_compose
// draws each window's material + title bar + a focus dot + content directly into the backbuffer.

TEST_CASE("compose paints desktop, a focused window's content + focus dot, and the cursor") {
	nw_server s; nw_server_init(&s, 200, 150);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	nw_msg cm{}; cm.type = NW_REQ_CREATE_WINDOW; cm.a = 140; cm.b = 30; cm.length = 1;
	nw_client_msg(&s, 0, &cm, (const unsigned char*) "w");
	int wi = s.focus;
	s.win[wi].x = 20; s.win[wi].y = 40;            // clear of the top panel

	std::vector<uint32_t> wbuf((size_t) 140 * 30, 0x00ABCDEF);
	s.win[wi].buf = wbuf.data();
	s.cursor_x = 188; s.cursor_y = 60;             // cursor away from the window

	std::vector<uint32_t> px((size_t) 200 * 150, 0);
	nw_surface back; back.px = px.data(); back.w = 200; back.h = 150; back.stride = 200;
	nw_surface_noclip(&back);
	nw_compose(&s, &back);

	auto at = [&](int x, int y) { return px[(size_t) y * 200 + x]; };
	CHECK(at(2, 40) != 0x00ABCDEFu);                                   // desktop, not window content
	CHECK(at(20 + 16, 40 + 14) == 0x12a8f4u);                          // focus dot = active blue
	CHECK(at(20 + NW_BORDER + 5, 40 + NW_TITLEBAR_H + 5) == 0x00ABCDEFu); // content colour
	CHECK(at(188, 60) == 0x101620u);                                    // cursor outline tip
}

TEST_CASE("unfocused window draws a muted focus dot; the focused one is active blue") {
	nw_server s; nw_server_init(&s, 300, 200);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	nw_msg cm{}; cm.type = NW_REQ_CREATE_WINDOW; cm.a = 90; cm.b = 30; cm.length = 1;
	nw_client_msg(&s, 0, &cm, (const unsigned char*) "a");
	int a = s.focus; s.win[a].x = 10; s.win[a].y = 40;
	nw_client_msg(&s, 0, &cm, (const unsigned char*) "b");
	int b = s.focus; s.win[b].x = 160; s.win[b].y = 70;
	CHECK(a != b);

	std::vector<uint32_t> px((size_t) 300 * 200, 0);
	nw_surface back; back.px = px.data(); back.w = 300; back.h = 200; back.stride = 300;
	nw_surface_noclip(&back);
	nw_compose(&s, &back);
	auto at = [&](int x, int y) { return px[(size_t) y * 300 + x]; };
	CHECK(at(10 + 16, 40 + 14) == 0x9fb2ccu);      // window a: muted (unfocused) dot
	CHECK(at(160 + 16, 70 + 14) == 0x12a8f4u);     // window b: active (focused) dot
}

// --- Phase 3: per-window frame cache must composite bit-identically to the live path --------

TEST_CASE("cached-frame compose equals fresh compose pixel-for-pixel (glass path)") {
	const int W = 320, H = 240;
	nw_server s; nw_server_init(&s, W, H);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());

	// Two windows with real content, overlapping, one focused — exercises chrome, blit, glass,
	// rounded corners, focus dim and z-order.
	nw_msg cm{}; cm.type = NW_REQ_CREATE_WINDOW; cm.a = 120; cm.b = 60; cm.length = 1;
	nw_client_msg(&s, 0, &cm, (const unsigned char*) "A");
	int a = s.focus; s.win[a].x = 30; s.win[a].y = 50;
	nw_client_msg(&s, 0, &cm, (const unsigned char*) "B");      // dark title (leading 0x01) too
	int b = s.focus; s.win[b].x = 110; s.win[b].y = 90;
	std::vector<uint32_t> ba((size_t) 120 * 60), bb((size_t) 120 * 60);
	for (size_t i = 0; i < ba.size(); i++) { ba[i] = (uint32_t) (0x203040 + i); bb[i] = (uint32_t) (0x70a0c0 ^ i); }
	s.win[a].buf = ba.data(); s.win[b].buf = bb.data();

	std::vector<uint32_t> scratch((size_t) W * H, 0);
	nw_surface sc; sc.px = scratch.data(); sc.w = W; sc.h = H; sc.stride = W; nw_surface_noclip(&sc);

	// FRESH: no frames -> live scratch+glass path.
	std::vector<uint32_t> pf((size_t) W * H, 0);
	nw_surface bf; bf.px = pf.data(); bf.w = W; bf.h = H; bf.stride = W; nw_surface_noclip(&bf);
	s.win[a].frame = s.win[b].frame = 0;
	nw_compose_scene(&s, &bf, &sc, 0, 0);

	// CACHED: allocate per-window frames, render them dirty, then compose from the cache.
	auto fw = [&](int i){ return s.win[i].cw + 2 * NW_BORDER; };
	auto fh = [&](int i){ return NW_TITLEBAR_H + s.win[i].ch + NW_BORDER; };
	std::vector<uint32_t> fa((size_t) fw(a) * fh(a)), fbuf((size_t) fw(b) * fh(b));
	s.win[a].frame = fa.data(); s.win[a].frame_dirty = 1;
	s.win[b].frame = fbuf.data(); s.win[b].frame_dirty = 1;
	nw_render_dirty_frames(&s);
	CHECK(s.win[a].frame_dirty == 0);                       // render cleared the flag
	CHECK(s.win[b].frame_dirty == 0);
	std::vector<uint32_t> pc((size_t) W * H, 0);
	nw_surface bc; bc.px = pc.data(); bc.w = W; bc.h = H; bc.stride = W; nw_surface_noclip(&bc);
	nw_compose_scene(&s, &bc, &sc, 0, 0);

	CHECK(pf == pc);                                        // bit-identical full scene
}

TEST_CASE("backdrop: glass window blurs a sharp edge in the scene below it") {
	// 200x200 scene whose wallpaper has a hard vertical edge at x=100 (black|white). A glass
	// window covers the centre; with the blurred backdrop the edge seen through the glass is
	// gradual (small left/right jump), without it the hard edge bleeds straight through.
	const int W = 200, H = 200;
	nw_server s; nw_server_init(&s, W, H);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	nw_msg cm{}; cm.type = NW_REQ_CREATE_WINDOW; cm.a = 120; cm.b = 120; cm.length = 1;
	nw_client_msg(&s, 0, &cm, (const unsigned char*) "g");
	int wi = s.focus; s.win[wi].x = 40; s.win[wi].y = 40;
	CHECK(s.win[wi].glass == 1);                              // glass on by default

	std::vector<uint32_t> scratch((size_t) W * H, 0);
	nw_surface sc; sc.px = scratch.data(); sc.w = W; sc.h = H; sc.stride = W; nw_surface_noclip(&sc);

	std::vector<uint32_t> wall((size_t) W * H, 0u);
	for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
		wall[(size_t) y * W + x] = x < 100 ? 0x00000000u : 0x00ffffffu;
	nw_surface wl; wl.px = wall.data(); wl.w = W; wl.h = H; wl.stride = W; nw_surface_noclip(&wl);

	std::vector<uint32_t> bd((size_t) W * H, 0u);
	std::vector<uint32_t> lo((size_t)(W / 4 + 1) * (H / 4 + 1) + 16, 0u);
	nw_surface bds; bds.px = bd.data(); bds.w = W; bds.h = H; bds.stride = W; nw_surface_noclip(&bds);
	nw_backdrop_ctx ctx{}; ctx.bd = &bds; ctx.lo = lo.data(); ctx.lo_cap = (int) lo.size();
	ctx.factor = 4; ctx.radius = NW_BD_BLUR_RADIUS; ctx.passes = NW_BD_BLUR_PASSES;
	ctx.drag_win = -1;                               // not dragging in this test
	nw_compose_set_theme(0x12a8f4u, 11, 0);          // isolate the blur: no drop-shadow confound

	std::vector<uint32_t> pa((size_t) W * H, 0), pb((size_t) W * H, 0);
	nw_surface backA; backA.px = pa.data(); backA.w = W; backA.h = H; backA.stride = W; nw_surface_noclip(&backA);
	nw_surface backB; backB.px = pb.data(); backB.w = W; backB.h = H; backB.stride = W; nw_surface_noclip(&backB);

	nw_compose_scene(&s, &backA, &sc, &wl, 0);      // no blur: hard edge bleeds through the glass
	nw_compose_scene(&s, &backB, &sc, &wl, &ctx);   // blurred backdrop: soft edge

	auto chan = [](uint32_t c){ return (int)((c >> 16) & 0xff); };
	auto iabs = [](int v){ return v < 0 ? -v : v; };
	int yrow = 100;
	int jumpA = iabs(chan(pa[(size_t) yrow * W + 98]) - chan(pa[(size_t) yrow * W + 102]));
	int jumpB = iabs(chan(pb[(size_t) yrow * W + 98]) - chan(pb[(size_t) yrow * W + 102]));
	CHECK(jumpB < jumpA);                            // blur softened the edge seen through the glass
	nw_compose_set_theme(0x12a8f4u, 11, 1);          // restore default theme for any later test
}

TEST_CASE("wallpaper: diagonal gradient with four colour blobs, deterministic") {
	const int W = 120, H = 90;
	std::vector<uint32_t> px((size_t) W * H, 0xdeadbeef);
	nw_surface d; d.px = px.data(); d.w = W; d.h = H; d.stride = W; nw_surface_noclip(&d);
	nw_render_wallpaper(&d);
	auto at = [&](int x, int y) { return px[(size_t) y * W + x]; };
	CHECK(at(0, 0) != 0xdeadbeefu);                 // every pixel written
	CHECK(at(W - 1, H - 1) != 0xdeadbeefu);
	// top row starts at the cool end of the gradient, bottom at the warm end
	CHECK(((at(W - 1, 0) >> 16) & 0xff) < ((at(W - 1, H - 1) >> 16) & 0xff));
	// the blue blob's centre pulls the red channel well below the plain warm-gradient corner
	// (bottom-right lies outside every blob at this surface size)
	int bx = W * 14 / 100, by = H * 23 / 100;
	CHECK(((at(bx, by) >> 16) & 0xff) < ((at(W - 1, H - 1) >> 16) & 0xff));
	// same seed -> same pixels (pure function of the surface size)
	std::vector<uint32_t> px2((size_t) W * H, 0);
	nw_surface d2; d2.px = px2.data(); d2.w = W; d2.h = H; d2.stride = W; nw_surface_noclip(&d2);
	nw_render_wallpaper(&d2);
	CHECK(px == px2);
}

TEST_CASE("glass frame: band renders key-black with the glowing centred title ink") {
	const int W = 260, H = 180;
	nw_server s; nw_server_init(&s, W, H);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	nw_msg cm{}; cm.type = NW_REQ_CREATE_WINDOW; cm.a = 140; cm.b = 60; cm.length = 5;
	nw_client_msg(&s, 0, &cm, (const unsigned char*) "Title");
	int wi = s.focus; s.win[wi].x = 40; s.win[wi].y = 50;
	std::vector<uint32_t> wbuf((size_t) 140 * 60, 0x00334455);
	s.win[wi].buf = wbuf.data();

	std::vector<uint32_t> pa((size_t) W * H, 0), pb((size_t) W * H, 0);
	nw_surface backA; backA.px = pa.data(); backA.w = W; backA.h = H; backA.stride = W; nw_surface_noclip(&backA);
	nw_surface backB; backB.px = pb.data(); backB.w = W; backB.h = H; backB.stride = W; nw_surface_noclip(&backB);

	nw_compose_set_glass_frame(0); nw_compose(&s, &backA);   // classic frame
	nw_compose_set_glass_frame(1);
	s.win[wi].frame_dirty = 1;                               // theme change invalidates caches
	nw_compose(&s, &backB);                                  // GL glass frame band
	nw_compose_set_glass_frame(0);                           // restore for later tests

	auto at = [&](std::vector<uint32_t> &p, int x, int y) { return p[(size_t) y * W + x]; };
	// a frame-band corner pixel: pure key black on the glass path, opaque chrome on classic
	int fx = 40 + 2, fy = 50 + 2;
	CHECK(at(pb, fx, fy) == 0x000000u);
	CHECK(at(pa, fx, fy) != at(pb, fx, fy));
	// the centred title ink glows: some non-black pixel exists in the middle of the titlebar
	int found = 0, cy = 50 + NW_TITLEBAR_H / 2;
	for (int x = 40; x < 40 + 140 && !found; x++) if (at(pb, x, cy) != 0x000000u) found = 1;
	CHECK(found == 1);
}

TEST_CASE("run launcher chrome: prompt panel + typed text + caret render above the desktop") {
	const int W = 320, H = 200;
	nw_server s; nw_server_init(&s, W, H);
	s.run_open = 1;
	const char *txt = "doom";
	s.run_len = 4; std::memcpy(s.run_text, txt, 4);
	std::vector<uint32_t> px((size_t) W * H, 0);
	nw_surface back; back.px = px.data(); back.w = W; back.h = H; back.stride = W; nw_surface_noclip(&back);
	nw_compose(&s, &back);
	int x, y, w, h; nw_run_rect(&s, &x, &y, &w, &h);
	auto at = [&](int xx, int yy) { return px[(size_t) yy * W + xx]; };
	CHECK(at(x + w / 2, y + 2) != at(0, H - 1));   // panel body differs from bare desktop
	// the "Run:" label ink appears somewhere on the panel's text row
	int ink = 0, ty = y + h / 2;
	for (int xx = x + 4; xx < x + w - 4 && !ink; xx++) if (at(xx, ty) == 0x9fb0c0u || at(xx, ty) == 0xffffffu) ink = 1;
	CHECK(ink == 1);
}

TEST_CASE("auth dialog chrome: dimmed desktop, panel, password dots and both buttons") {
	const int W = 420, H = 300;
	nw_server s; nw_server_init(&s, W, H);
	s.auth_open = 1;
	std::strcpy(s.auth_cmd, "/nanos/bin/sysconf.nxe");
	s.auth_passlen = 3; s.auth_hover = 0;
	std::vector<uint32_t> px((size_t) W * H, 0), px2((size_t) W * H, 0);
	nw_surface back; back.px = px.data(); back.w = W; back.h = H; back.stride = W; nw_surface_noclip(&back);
	nw_compose(&s, &back);
	int x, y, w, h; nw_auth_rect(&s, &x, &y, &w, &h);
	auto at = [&](std::vector<uint32_t> &p, int xx, int yy) { return p[(size_t) yy * W + xx]; };
	CHECK(at(px, x + w / 2, y + 4) != 0x000000u);            // panel body drawn
	int bx, by, bw, bh; nw_auth_btn_rect(&s, 0, &bx, &by, &bw, &bh);
	CHECK(at(px, bx + 3, by + 3) == 0x0060dfu);              // hovered Authenticate = darker accent
	// hover the other button instead: Cancel picks the hover fill, Authenticate the plain accent
	s.auth_hover = 1;
	nw_surface back2; back2.px = px2.data(); back2.w = W; back2.h = H; back2.stride = W; nw_surface_noclip(&back2);
	nw_compose(&s, &back2);
	CHECK(at(px2, bx + 3, by + 3) == 0x0a84ffu);
	int cx, cy2, cw, ch2; nw_auth_btn_rect(&s, 1, &cx, &cy2, &cw, &ch2);
	CHECK(at(px2, cx + 3, cy2 + 3) == 0xdbe1eau);
}

TEST_CASE("nw_compose_chrome: overlay keyed black with only the chrome painted") {
	const int W = 300, H = 220;
	nw_server s; nw_server_init(&s, W, H);
	std::vector<uint32_t> px((size_t) W * H, 0xffffffff);
	nw_surface ov; ov.px = px.data(); ov.w = W; ov.h = H; ov.stride = W; nw_surface_noclip(&ov);
	nw_compose_chrome(&s, &ov);
	auto at = [&](int x, int y) { return px[(size_t) y * W + x]; };
	CHECK(at(W / 2, H / 2) == 0x000000u);          // mid-desktop = transparent key
	int chrome = 0;                                 // top panel and/or taskbar painted something
	for (int x = 0; x < W && !chrome; x++)
		if (at(x, 2) != 0x000000u || at(x, H - 3) != 0x000000u) chrome = 1;
	CHECK(chrome == 1);
}

TEST_CASE("menu dropdown renders items under an open menu") {
	const int W = 320, H = 240;
	nw_server s; nw_server_init(&s, W, H);
	s.menu_open = 1; s.menu_which = 0; s.menu_hover = 0;
	std::vector<uint32_t> pa((size_t) W * H, 0), pb((size_t) W * H, 0);
	nw_surface backA; backA.px = pa.data(); backA.w = W; backA.h = H; backA.stride = W; nw_surface_noclip(&backA);
	nw_surface backB; backB.px = pb.data(); backB.w = W; backB.h = H; backB.stride = W; nw_surface_noclip(&backB);
	nw_compose(&s, &backA);
	s.menu_open = 0;
	nw_compose(&s, &backB);
	CHECK(pa != pb);                                // the dropdown painted pixels the closed menu lacks
}

TEST_CASE("moving a cached window does NOT dirty its frame (drag is re-render-free)") {
	nw_server s; nw_server_init(&s, 200, 200);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	nw_msg cm{}; cm.type = NW_REQ_CREATE_WINDOW; cm.a = 80; cm.b = 40; cm.length = 1;
	nw_client_msg(&s, 0, &cm, (const unsigned char*) "w");
	int wi = s.focus;
	std::vector<uint32_t> fr((size_t)(80 + 2 * NW_BORDER) * (NW_TITLEBAR_H + 40 + NW_BORDER));
	s.win[wi].frame = fr.data();
	nw_render_dirty_frames(&s);                 // first render clears the create-dirty
	CHECK(s.win[wi].frame_dirty == 0);
	s.win[wi].x += 25; s.win[wi].y += 17;       // a move, as the drag handler does
	CHECK(s.win[wi].frame_dirty == 0);          // still clean -> compose reuses the cached frame
}
