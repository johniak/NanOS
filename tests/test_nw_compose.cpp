#include "doctest.h"
#include "nw_compose.h"
#include "nwm_core.h"
#include "nwproto.h"
#include <vector>
#include <cstring>

// Compose a one-window desktop into an in-memory backbuffer and assert pixels.
TEST_CASE("compose paints desktop, a focused window's title/content, and the cursor") {
	nw_server s; nw_server_init(&s, 200, 150);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	nw_msg cm{}; cm.type = NW_REQ_CREATE_WINDOW; cm.a = 40; cm.b = 30; cm.length = 1;
	nw_client_msg(&s, 0, &cm, (const unsigned char*) "w");
	int wi = s.focus;
	s.win[wi].x = 20; s.win[wi].y = 20;

	// fill the window content buffer with a distinctive color
	std::vector<uint32_t> wbuf((size_t) 40 * 30, 0x00ABCDEF);
	s.win[wi].buf = wbuf.data();
	s.cursor_x = 180; s.cursor_y = 140;            // cursor in the far corner (won't overlap)

	std::vector<uint32_t> px((size_t) 200 * 150, 0);
	nw_surface back; back.px = px.data(); back.w = 200; back.h = 150; back.stride = 200;
	nw_compose(&s, &back);

	auto at = [&](int x, int y) { return px[(size_t) y * 200 + x]; };
	// desktop background somewhere empty
	CHECK(at(2, 2) != 0x00ABCDEFu);
	// title bar of the window (focused color, near its top-left)
	CHECK(at(20 + 4, 20 + 4) == 0x3a78c0u);
	// content shows the committed window color
	CHECK(at(20 + NW_BORDER + 5, 20 + NW_TITLEBAR_H + 5) == 0x00ABCDEFu);
	// cursor tip pixel is the outline color
	CHECK(at(180, 140) == 0x000000u);
}

TEST_CASE("unfocused window draws with the inactive title color, focused on top") {
	nw_server s; nw_server_init(&s, 300, 200);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	nw_msg cm{}; cm.type = NW_REQ_CREATE_WINDOW; cm.a = 50; cm.b = 30; cm.length = 1;
	nw_client_msg(&s, 0, &cm, (const unsigned char*) "a");
	int a = s.focus; s.win[a].x = 10; s.win[a].y = 10;
	nw_client_msg(&s, 0, &cm, (const unsigned char*) "b");
	int b = s.focus; s.win[b].x = 100; s.win[b].y = 100;
	CHECK(a != b);

	std::vector<uint32_t> px((size_t) 300 * 200, 0);
	nw_surface back; back.px = px.data(); back.w = 300; back.h = 200; back.stride = 300;
	nw_compose(&s, &back);
	auto at = [&](int x, int y) { return px[(size_t) y * 300 + x]; };
	CHECK(at(10 + 4, 10 + 4) == 0x586070u);        // window a: unfocused title
	CHECK(at(100 + 4, 100 + 4) == 0x3a78c0u);      // window b: focused title
}
