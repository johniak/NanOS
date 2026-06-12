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
	nw_compose_scene(&s, &bf, &sc, 0);

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
	nw_compose_scene(&s, &bc, &sc, 0);

	CHECK(pf == pc);                                        // bit-identical full scene
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
