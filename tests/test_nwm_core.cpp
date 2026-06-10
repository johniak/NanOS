#include "doctest.h"
#include "nwm_core.h"
#include "nwproto.h"
#include <vector>
#include <cstring>

// Drain every byte queued for a client and decode it back into events.
struct Ev { nw_msg m; std::vector<unsigned char> pay; };
static std::vector<Ev> drain(nw_server& s, int client) {
	std::vector<unsigned char> bytes;
	for (;;) {
		uint32_t len = 0;
		const unsigned char* p = nw_outq_peek(&s, client, &len);
		if (!len) break;
		bytes.insert(bytes.end(), p, p + len);
		nw_outq_ack(&s, client, len);
	}
	std::vector<unsigned char> paybuf(8192);
	nw_decoder d; nw_decoder_init(&d, paybuf.data(), paybuf.size());
	std::vector<Ev> out;
	const unsigned char* p = bytes.data();
	const unsigned char* end = p + bytes.size();
	while (nw_decoder_next(&d, &p, end)) {
		Ev e; e.m = d.msg;
		uint32_t k = d.msg.length < 8192 ? d.msg.length : 8192;
		e.pay.assign(d.payload, d.payload + k);
		out.push_back(e);
	}
	return out;
}
static int count(const std::vector<Ev>& v, uint32_t type) {
	int n = 0; for (auto& e : v) if (e.m.type == type) n++; return n;
}
static const Ev* last(const std::vector<Ev>& v, uint32_t type) {
	const Ev* r = nullptr; for (auto& e : v) if (e.m.type == type) r = &e; return r;
}
static uint32_t create_win(nw_server& s, int client, int w, int h, const char* title) {
	nw_msg m{}; m.type = NW_REQ_CREATE_WINDOW; m.a = w; m.b = h; m.length = (uint32_t) strlen(title);
	nw_client_msg(&s, client, &m, (const unsigned char*) title);
	return s.win[s.focus].id;
}

TEST_CASE("create window emits CONFIGURE and focuses it") {
	nw_server s; nw_server_init(&s, 800, 600);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	uint32_t id = create_win(s, 0, 200, 100, "hello");
	auto ev = drain(s, 0);
	const Ev* cfg = last(ev, NW_EVT_CONFIGURE);
	REQUIRE(cfg);
	CHECK(cfg->m.window == id);
	CHECK(cfg->m.a == 200);
	CHECK(cfg->m.b == 100);
	CHECK(s.focus >= 0);
	CHECK(s.win[s.focus].id == id);
	CHECK(strcmp(s.win[s.focus].title, "hello") == 0);
}

TEST_CASE("hit test classifies title / content / close / none") {
	nw_server s; nw_server_init(&s, 800, 600);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	create_win(s, 0, 200, 100, "w");
	int wi = s.focus;
	s.win[wi].x = 100; s.win[wi].y = 100;
	int region = -1;
	// title bar
	CHECK(nw_hit(&s, 110, 105, &region) == wi); CHECK(region == NW_HIT_TITLE);
	// content (origin = x+BORDER, y+TITLEBAR_H)
	CHECK(nw_hit(&s, 100 + NW_BORDER + 5, 100 + NW_TITLEBAR_H + 5, &region) == wi);
	CHECK(region == NW_HIT_CONTENT);
	// close box (top-right of title bar)
	int fw = 200 + 2 * NW_BORDER;
	int cbx = 100 + fw - NW_BORDER - NW_CLOSE - 2 + 3;
	int cby = 100 + (NW_TITLEBAR_H - NW_CLOSE) / 2 + 3;
	CHECK(nw_hit(&s, cbx, cby, &region) == wi); CHECK(region == NW_HIT_CLOSE);
	// empty desktop
	CHECK(nw_hit(&s, 700, 500, &region) == -1); CHECK(region == NW_HIT_NONE);
}

TEST_CASE("click on content focuses and delivers a window-relative POINTER") {
	nw_server s; nw_server_init(&s, 800, 600);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	create_win(s, 0, 200, 100, "w");
	int wi = s.focus; s.win[wi].x = 50; s.win[wi].y = 60;
	drain(s, 0);                                   // clear CONFIGURE/FOCUS
	int px = 50 + NW_BORDER + 7, py = 60 + NW_TITLEBAR_H + 9;
	nw_pointer(&s, px, py, NW_BTN_LEFT);
	auto ev = drain(s, 0);
	const Ev* ptr = last(ev, NW_EVT_POINTER);
	REQUIRE(ptr);
	CHECK(ptr->m.a == 7);                          // window-relative x
	CHECK(ptr->m.b == 9);                          // window-relative y
	CHECK(ptr->m.c == NW_BTN_LEFT);
}

TEST_CASE("dragging the title bar moves the window") {
	nw_server s; nw_server_init(&s, 800, 600);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	create_win(s, 0, 200, 100, "w");
	int wi = s.focus; s.win[wi].x = 100; s.win[wi].y = 100;
	nw_pointer(&s, 120, 108, NW_BTN_LEFT);         // press on title bar -> grab
	CHECK(s.drag_win == wi);
	nw_pointer(&s, 170, 158, NW_BTN_LEFT);         // move +50,+50
	CHECK(s.win[wi].x == 150);
	CHECK(s.win[wi].y == 150);
	nw_pointer(&s, 170, 158, 0);                    // release
	CHECK(s.drag_win == -1);
}

TEST_CASE("click on close box sends CLOSE to the owning client") {
	nw_server s; nw_server_init(&s, 800, 600);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	uint32_t id = create_win(s, 0, 200, 100, "w");
	int wi = s.focus; s.win[wi].x = 100; s.win[wi].y = 100;
	drain(s, 0);
	int fw = 200 + 2 * NW_BORDER;
	int cbx = 100 + fw - NW_BORDER - NW_CLOSE - 2 + 3;
	int cby = 100 + (NW_TITLEBAR_H - NW_CLOSE) / 2 + 3;
	nw_pointer(&s, cbx, cby, NW_BTN_LEFT);
	auto ev = drain(s, 0);
	const Ev* cl = last(ev, NW_EVT_CLOSE);
	REQUIRE(cl);
	CHECK(cl->m.window == id);
}

TEST_CASE("normal keys decode to ASCII (with shift) and route to the focused window") {
	nw_server s; nw_server_init(&s, 800, 600);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	create_win(s, 0, 100, 100, "w");
	drain(s, 0);
	nw_key(&s, 0x1E, 1);                            // 'a'
	nw_key(&s, NW_SC_LSHIFT, 1);
	nw_key(&s, 0x1E, 1);                            // 'A'
	auto ev = drain(s, 0);
	REQUIRE(count(ev, NW_EVT_KEY) == 2);
	CHECK(ev[0].m.a == 'a');
	CHECK(ev[1].m.a == 'A');
	CHECK(ev[0].m.b == 1);                          // down
	CHECK(ev[0].m.c == 0x1E);                       // raw scancode preserved
}

TEST_CASE("Super shortcuts: Q closes, C/X copy/cut, V pastes the clipboard, and are consumed") {
	nw_server s; nw_server_init(&s, 800, 600);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	uint32_t id = create_win(s, 0, 100, 100, "w");
	// set the clipboard via a client request
	const char* clip = "PASTED";
	nw_msg sc{}; sc.type = NW_REQ_SET_CLIPBOARD; sc.length = 6;
	nw_client_msg(&s, 0, &sc, (const unsigned char*) clip);
	drain(s, 0);

	nw_key(&s, NW_SC_LSUPER, 1);
	nw_key(&s, NW_SC_C, 1);                         // Super+C -> COPY a=0
	nw_key(&s, NW_SC_X, 1);                         // Super+X -> COPY a=1 (cut)
	nw_key(&s, NW_SC_V, 1);                         // Super+V -> PASTE payload
	nw_key(&s, NW_SC_Q, 1);                         // Super+Q -> CLOSE
	auto ev = drain(s, 0);
	REQUIRE(count(ev, NW_EVT_KEY) == 0);            // shortcuts never leak as key events
	REQUIRE(count(ev, NW_EVT_COPY) == 2);
	CHECK(ev[0].m.a == 0);                          // copy
	CHECK(ev[1].m.a == 1);                          // cut
	const Ev* paste = last(ev, NW_EVT_PASTE);
	REQUIRE(paste);
	REQUIRE(paste->pay.size() == 6);
	CHECK(memcmp(paste->pay.data(), "PASTED", 6) == 0);
	const Ev* cl = last(ev, NW_EVT_CLOSE);
	REQUIRE(cl); CHECK(cl->m.window == id);
}

TEST_CASE("Super+Tab cycles focus and emits FOCUS in/out") {
	nw_server s; nw_server_init(&s, 800, 600);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	uint32_t id1 = create_win(s, 0, 100, 100, "a");
	uint32_t id2 = create_win(s, 0, 100, 100, "b");
	CHECK(s.win[s.focus].id == id2);               // newest is focused
	drain(s, 0);
	nw_key(&s, NW_SC_LSUPER, 1);
	nw_key(&s, NW_SC_TAB, 1);
	CHECK(s.win[s.focus].id == id1);               // cycled to the other window
	auto ev = drain(s, 0);
	CHECK(count(ev, NW_EVT_FOCUS) == 2);           // out of id2, into id1
}

TEST_CASE("COMMIT copies pixels into the window buffer at the damage rect") {
	nw_server s; nw_server_init(&s, 800, 600);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	create_win(s, 0, 4, 4, "w");
	int wi = s.focus;
	std::vector<uint32_t> wbuf(16, 0);
	s.win[wi].buf = wbuf.data();
	// commit a 2x2 block at (1,1)
	uint32_t pix[4] = { 0x11, 0x22, 0x33, 0x44 };
	nw_msg cm{}; cm.type = NW_REQ_COMMIT; cm.window = s.win[wi].id;
	cm.a = 1; cm.b = 1; cm.c = 2; cm.d = 2; cm.length = sizeof pix;
	nw_client_msg(&s, 0, &cm, (const unsigned char*) pix);
	CHECK(wbuf[1 * 4 + 1] == 0x11u);
	CHECK(wbuf[1 * 4 + 2] == 0x22u);
	CHECK(wbuf[2 * 4 + 1] == 0x33u);
	CHECK(wbuf[2 * 4 + 2] == 0x44u);
	CHECK(wbuf[0] == 0u);                           // untouched
}

TEST_CASE("output ring overflow marks the client dead, never writes a partial message") {
	nw_server s; nw_server_init(&s, 800, 600);
	unsigned char tiny[40];                          // < 2 messages worth
	nw_client_connect(&s, 0, tiny, sizeof tiny);
	create_win(s, 0, 10, 10, "w");                   // CONFIGURE (+FOCUS) fits the first, then...
	for (int i = 0; i < 20; i++) nw_key(&s, 0x1E, 1);// floods events
	CHECK(nw_client_is_dead(&s, 0));
	// whatever bytes are queued must be a whole number of decodable messages
	auto ev = drain(s, 0);
	CHECK(ev.size() >= 1);
}

TEST_CASE("scancode keymap: full sweep plus representative mappings") {
	// Touch every code (covers all switch arms + default) for both shift states.
	for (int c = 0; c < 256; c++) { nw_scancode_ascii((unsigned char) c, 0); nw_scancode_ascii((unsigned char) c, 1); }
	CHECK(nw_scancode_ascii(0x02, 0) == '1');
	CHECK(nw_scancode_ascii(0x02, 1) == '!');
	CHECK(nw_scancode_ascii(0x1E, 0) == 'a');
	CHECK(nw_scancode_ascii(0x1E, 1) == 'A');
	CHECK(nw_scancode_ascii(0x35, 0) == '/');
	CHECK(nw_scancode_ascii(0x35, 1) == '?');
	CHECK(nw_scancode_ascii(0x39, 0) == ' ');
	CHECK(nw_scancode_ascii(0x0F, 0) == '\t');
	CHECK(nw_scancode_ascii(0x1C, 0) == '\n');
	CHECK(nw_scancode_ascii(0x0E, 0) == 8);
	CHECK(nw_scancode_ascii(0x80 | 0x1E, 0) == 0);   // extended -> no ASCII
	CHECK(nw_scancode_ascii(0x70, 0) == 0);          // unmapped -> 0
}

TEST_CASE("HELLO is a no-op and DESTROY_WINDOW removes the window") {
	nw_server s; nw_server_init(&s, 800, 600);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	nw_msg h{}; h.type = NW_REQ_HELLO; h.a = NW_PROTO_VERSION;
	nw_client_msg(&s, 0, &h, nullptr);
	auto ev0 = drain(s, 0);
	CHECK(ev0.empty());
	uint32_t id = create_win(s, 0, 100, 100, "w");
	CHECK(s.zn == 1);
	nw_msg dm{}; dm.type = NW_REQ_DESTROY_WINDOW; dm.window = id;
	nw_client_msg(&s, 0, &dm, nullptr);
	CHECK(s.zn == 0);
	CHECK(s.focus == -1);
}

TEST_CASE("COMMIT with a too-short payload is ignored; out-of-window rect clips") {
	nw_server s; nw_server_init(&s, 800, 600);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	create_win(s, 0, 4, 4, "w");
	int wi = s.focus;
	std::vector<uint32_t> wbuf(16, 0x55);
	s.win[wi].buf = wbuf.data();
	// claims 2x2 but provides only 4 bytes (< 16): rejected wholesale
	uint32_t one = 0x99;
	nw_msg cm{}; cm.type = NW_REQ_COMMIT; cm.window = s.win[wi].id;
	cm.a = 0; cm.b = 0; cm.c = 2; cm.d = 2; cm.length = 4;
	nw_client_msg(&s, 0, &cm, (const unsigned char*) &one);
	for (auto v : wbuf) CHECK(v == 0x55u);          // untouched
	// commit a 2x2 partly off the right/bottom edge: only the in-bounds pixel lands
	uint32_t pix[4] = { 0xA, 0xB, 0xC, 0xD };
	cm.a = 3; cm.b = 3; cm.c = 2; cm.d = 2; cm.length = sizeof pix;
	nw_client_msg(&s, 0, &cm, (const unsigned char*) pix);
	CHECK(wbuf[3 * 4 + 3] == 0xAu);                 // (3,3) in-bounds
}

TEST_CASE("nw_window_needs_buffer reports unbuffered windows until bound") {
	nw_server s; nw_server_init(&s, 800, 600);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	create_win(s, 0, 8, 8, "w");
	nw_window* nb = nw_window_needs_buffer(&s);
	REQUIRE(nb);
	std::vector<uint32_t> wbuf(64, 0);
	nb->buf = wbuf.data();
	CHECK(nw_window_needs_buffer(&s) == nullptr);
}

TEST_CASE("clicking the empty desktop clears focus") {
	nw_server s; nw_server_init(&s, 800, 600);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	create_win(s, 0, 50, 50, "w");
	s.win[s.focus].x = 10; s.win[s.focus].y = 10;
	drain(s, 0);
	nw_pointer(&s, 700, 500, NW_BTN_LEFT);          // empty desktop
	CHECK(s.focus == -1);
	auto ev = drain(s, 0);
	CHECK(count(ev, NW_EVT_FOCUS) == 1);            // FOCUS-out of the old window
}

TEST_CASE("scene damage: create/commit mark a rect; a plain cursor move does not") {
	nw_server s; nw_server_init(&s, 800, 600);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	int x, y, w, h;
	CHECK(nw_take_damage(&s, &x, &y, &w, &h) == 0);   // fresh: nothing damaged
	create_win(s, 0, 100, 80, "w");
	CHECK(nw_take_damage(&s, &x, &y, &w, &h) == 1);    // create marked damage
	CHECK(w > 0); CHECK(h > 0);
	CHECK(nw_take_damage(&s, &x, &y, &w, &h) == 0);    // taking it cleared it
	nw_pointer(&s, 400, 300, 0);                        // cursor-only move
	CHECK(nw_take_damage(&s, &x, &y, &w, &h) == 0);    // overlay: no scene damage
	int wi = s.focus;
	std::vector<uint32_t> buf((size_t) 100 * 80, 0);
	s.win[wi].buf = buf.data();
	uint32_t px[4] = { 1, 2, 3, 4 };
	nw_msg cm{}; cm.type = NW_REQ_COMMIT; cm.window = s.win[wi].id;
	cm.a = 0; cm.b = 0; cm.c = 2; cm.d = 2; cm.length = sizeof px;
	nw_client_msg(&s, 0, &cm, (const unsigned char*) px);
	CHECK(nw_take_damage(&s, &x, &y, &w, &h) == 1);    // commit marked the damaged rect
}

TEST_CASE("nw_peek_damage reads the damage rect without clearing it") {
	nw_server s; nw_server_init(&s, 800, 600);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	int x, y, w, h;
	CHECK(nw_peek_damage(&s, &x, &y, &w, &h) == 0);    // fresh: nothing to peek
	create_win(s, 0, 100, 80, "w");
	int px, py, pw, ph;
	CHECK(nw_peek_damage(&s, &px, &py, &pw, &ph) == 1);  // peek sees the create damage
	CHECK(nw_peek_damage(&s, &x, &y, &w, &h) == 1);      // ...and a second peek still sees it
	CHECK(x == px); CHECK(y == py); CHECK(w == pw); CHECK(h == ph);  // same box, not cleared
	CHECK(nw_take_damage(&s, &x, &y, &w, &h) == 1);      // take returns the same box
	CHECK(x == px); CHECK(y == py); CHECK(w == pw); CHECK(h == ph);
	CHECK(nw_peek_damage(&s, &x, &y, &w, &h) == 0);      // now cleared
}

TEST_CASE("panel buttons hit-test and clicks set the quit/shutdown flags") {
	nw_server s; nw_server_init(&s, 800, 600);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	int x, y, w, h;
	nw_panel_button_rect(&s, NW_PANEL_QUIT, &x, &y, &w, &h);
	CHECK(nw_panel_hit(&s, x + 1, y + 1) == NW_PANEL_QUIT);
	nw_panel_button_rect(&s, NW_PANEL_SHUTDOWN, &x, &y, &w, &h);
	CHECK(nw_panel_hit(&s, x + 1, y + 1) == NW_PANEL_SHUTDOWN);
	CHECK(nw_panel_hit(&s, 5, 5) == NW_PANEL_NONE);        // empty part of the bar
	CHECK(nw_panel_hit(&s, 400, 400) == NW_PANEL_NONE);    // below the bar

	nw_panel_button_rect(&s, NW_PANEL_QUIT, &x, &y, &w, &h);
	nw_pointer(&s, x + 2, y + 2, NW_BTN_LEFT);
	CHECK(s.want_quit == 1);

	nw_server s2; nw_server_init(&s2, 800, 600);
	std::vector<unsigned char> ob2(8192); nw_client_connect(&s2, 0, ob2.data(), ob2.size());
	nw_panel_button_rect(&s2, NW_PANEL_SHUTDOWN, &x, &y, &w, &h);
	nw_pointer(&s2, x + 2, y + 2, NW_BTN_LEFT);
	CHECK(s2.want_shutdown == 1);
}

TEST_CASE("Super+R run dialog: type then Enter launches; Escape cancels") {
	nw_server s; nw_server_init(&s, 800, 600);
	nw_key(&s, NW_SC_LSUPER, 1);
	nw_key(&s, NW_SC_R, 1);                  // Super+R opens
	CHECK(s.run_open == 1);
	nw_key(&s, NW_SC_LSUPER, 0);
	nw_key(&s, 0x26, 1);                     // 'l'
	nw_key(&s, 0x1F, 1);                     // 's'
	CHECK(s.run_len == 2);
	nw_key(&s, NW_SC_BACKSP, 1);
	CHECK(s.run_len == 1);
	nw_key(&s, NW_SC_ENTER, 1);              // commit
	CHECK(s.run_open == 0);
	char out[64];
	CHECK(nw_run_take_spawn(&s, out, sizeof out) == 1);
	CHECK(strcmp(out, "l") == 0);
	CHECK(nw_run_take_spawn(&s, out, sizeof out) == 0);   // cleared after taking
	// Escape cancels without launching
	nw_key(&s, NW_SC_LSUPER, 1); nw_key(&s, NW_SC_R, 1); nw_key(&s, NW_SC_LSUPER, 0);
	CHECK(s.run_open == 1);
	nw_key(&s, NW_SC_ESC, 1);
	CHECK(s.run_open == 0);
	CHECK(nw_run_take_spawn(&s, out, sizeof out) == 0);
}

TEST_CASE("GET_CLIPBOARD replies with the stored clipboard as a PASTE to the focused window") {
	nw_server s; nw_server_init(&s, 800, 600);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	uint32_t id = create_win(s, 0, 100, 100, "w");
	nw_msg sc{}; sc.type = NW_REQ_SET_CLIPBOARD; sc.length = 4;
	nw_client_msg(&s, 0, &sc, (const unsigned char*) "CLIP");
	drain(s, 0);
	nw_msg gc{}; gc.type = NW_REQ_GET_CLIPBOARD;
	nw_client_msg(&s, 0, &gc, nullptr);
	auto ev = drain(s, 0);
	const Ev* p = last(ev, NW_EVT_PASTE);
	REQUIRE(p);
	REQUIRE(p->pay.size() == 4);
	CHECK(memcmp(p->pay.data(), "CLIP", 4) == 0);
	CHECK(p->m.window == id);
}

TEST_CASE("KEY events carry the shift modifier in field d") {
	nw_server s; nw_server_init(&s, 800, 600);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	create_win(s, 0, 100, 100, "w");
	drain(s, 0);
	nw_key(&s, NW_SC_LSHIFT, 1);                    // shift down (consumed)
	nw_key(&s, 0x1E, 1);                            // 'a' + shift -> 'A', d=1
	auto ev = drain(s, 0);
	const Ev* k = last(ev, NW_EVT_KEY);
	REQUIRE(k);
	CHECK(k->m.a == 'A');
	CHECK(k->m.d == 1);
}

TEST_CASE("disconnect drops the client's windows") {
	nw_server s; nw_server_init(&s, 800, 600);
	std::vector<unsigned char> ob(8192); nw_client_connect(&s, 0, ob.data(), ob.size());
	create_win(s, 0, 100, 100, "a");
	create_win(s, 0, 100, 100, "b");
	CHECK(s.zn == 2);
	nw_client_disconnect(&s, 0);
	CHECK(s.zn == 0);
	CHECK(s.focus == -1);
}
