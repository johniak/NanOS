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
