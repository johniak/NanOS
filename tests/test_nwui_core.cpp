#include "doctest.h"
#include "nwui_core.h"
#include "nwui.h"
#include "libnw.h"
#include <cstring>

static int g_clicks;
static void on_click(nwui_node *, void *u) { (*(int *) u)++; }

static void pointer(nwui *u, int x, int y, int buttons) {
	nw_event e; memset(&e, 0, sizeof e);
	e.type = NW_EV_POINTER; e.x = x; e.y = y; e.buttons = buttons;
	nwui_dispatch(u, &e);
}
static void click(nwui *u, int x, int y) { pointer(u, x, y, 1); pointer(u, x, y, 0); }
static void key(nwui *u, char ch) {
	nw_event e; memset(&e, 0, sizeof e);
	e.type = NW_EV_KEY; e.down = 1; e.ch = ch;
	nwui_dispatch(u, &e);
}

TEST_CASE("column layout stacks children, stretches them to width, honors pad+gap") {
	nwui *u = new nwui; nwui_init(u);
	char tb[32] = "hi";
	nwui_node *lbl = nwui_label(u, "Name:");
	nwui_node *tf  = nwui_textfield(u, tb, sizeof tb, 0, 0);
	nwui_node *btn = nwui_button(u, "OK", on_click, &g_clicks);
	nwui_node *col = nwui_pad(nwui_gap(nwui_column(u, lbl, tf, btn, (nwui_node *) 0), 5), 10);
	nwui_set_root(u, col);
	u->win_w = 200; u->win_h = 150;
	nwui_layout(u);

	CHECK(col->w == 200);
	CHECK(lbl->x == 10);            // pad
	CHECK(lbl->w == 180);           // stretched to 200 - 2*pad
	CHECK(lbl->y == 10);
	CHECK(tf->y  == 10 + lbl->h + 5);   // gap
	CHECK(btn->y == tf->y + tf->h + 5);
	delete u;
}

TEST_CASE("flex child grows to fill leftover space on the main axis") {
	nwui *u = new nwui; nwui_init(u);
	char tb[8] = "";
	nwui_node *lbl = nwui_label(u, "x");          // mh = 16
	nwui_node *tf  = nwui_flex(nwui_textfield(u, tb, sizeof tb, 0, 0), 1);  // mh = 24 + leftover
	nwui_node *col = nwui_column(u, lbl, tf, (nwui_node *) 0);
	nwui_set_root(u, col);
	u->win_w = 100; u->win_h = 100;
	nwui_layout(u);
	// used = 16 + 24 = 40, leftover = 100 - 40 = 60 -> all to the single flex child
	CHECK(tf->h == 24 + 60);
	CHECK(lbl->h == 16);
	delete u;
}

TEST_CASE("row packs children horizontally") {
	nwui *u = new nwui; nwui_init(u);
	nwui_node *a = nwui_button(u, "A", 0, 0);     // mw = 8 + 20 = 28
	nwui_node *b = nwui_button(u, "B", 0, 0);
	nwui_node *row = nwui_gap(nwui_row(u, a, b, (nwui_node *) 0), 6);
	nwui_set_root(u, row);
	u->win_w = 200; u->win_h = 40;
	nwui_layout(u);
	CHECK(a->x == 0);
	CHECK(b->x == a->x + a->w + 6);
	CHECK(a->y == b->y);
	delete u;
}

TEST_CASE("hit test returns the deepest node under a point") {
	nwui *u = new nwui; nwui_init(u);
	nwui_node *btn = nwui_button(u, "OK", 0, 0);
	nwui_node *col = nwui_column(u, nwui_label(u, "t"), btn, (nwui_node *) 0);
	nwui_set_root(u, col);
	u->win_w = 100; u->win_h = 100;
	nwui_layout(u);
	nwui_node *hit = nwui_hit(u->root, btn->x + 2, btn->y + 2);
	CHECK(hit == btn);
	CHECK(nwui_hit(u->root, 99, 99) != btn);   // empty area -> a container/label, not the button
	delete u;
}

TEST_CASE("clicking a button fires its callback on release") {
	nwui *u = new nwui; nwui_init(u);
	g_clicks = 0;
	nwui_node *btn = nwui_button(u, "Go", on_click, &g_clicks);
	nwui_set_root(u, nwui_column(u, btn, (nwui_node *) 0));
	u->win_w = 120; u->win_h = 60;
	nwui_layout(u);
	pointer(u, btn->x + 3, btn->y + 3, 1);   // press
	CHECK(btn->pressed == 1);
	pointer(u, btn->x + 3, btn->y + 3, 0);   // release on the same node
	CHECK(g_clicks == 1);
	CHECK(btn->pressed == 0);
	// press then release OUTSIDE the button does not fire
	pointer(u, btn->x + 3, btn->y + 3, 1);
	pointer(u, 200, 200, 0);
	CHECK(g_clicks == 1);
	delete u;
}

TEST_CASE("textfield: click focuses, keys edit the app buffer, backspace + paste work") {
	nwui *u = new nwui; nwui_init(u);
	char tb[16] = "hi";
	nwui_node *tf = nwui_textfield(u, tb, sizeof tb, 0, 0);
	nwui_set_root(u, nwui_column(u, tf, (nwui_node *) 0));
	u->win_w = 200; u->win_h = 60;
	nwui_layout(u);
	click(u, tf->x + 4, tf->y + 4);
	CHECK(u->focus == tf);
	CHECK(tf->focused == 1);
	key(u, 'a');                             // caret at end (2) -> "hia"
	CHECK(strcmp(tb, "hia") == 0);
	key(u, 8);                               // backspace -> "hi"
	CHECK(strcmp(tb, "hi") == 0);
	nw_event p; memset(&p, 0, sizeof p);
	p.type = NW_EV_PASTE; p.text = "XY"; p.text_len = 2;
	nwui_dispatch(u, &p);                     // -> "hiXY"
	CHECK(strcmp(tb, "hiXY") == 0);
	nwui_set_text(tf, "");                    // programmatic clear resets the buffer + caret
	CHECK(strcmp(tb, "") == 0);
	CHECK(tf->tlen == 0);
	CHECK(tf->caret == 0);
	nwui_set_text(tf, "abc");
	CHECK(strcmp(tb, "abc") == 0);
	CHECK(tf->caret == 3);
	delete u;
}

TEST_CASE("set_text updates the caption and marks layout dirty") {
	nwui *u = new nwui; nwui_init(u);
	nwui_node *lbl = nwui_label(u, "a");
	nwui_set_root(u, lbl);
	u->win_w = 100; u->win_h = 30;
	nwui_layout(u);
	CHECK(u->layout_dirty == 0);
	nwui_set_text(lbl, "Hello!");
	CHECK(strcmp(nwui_get_text(lbl), "Hello!") == 0);
	CHECK(lbl->dirty == 1);
	CHECK(u->layout_dirty == 1);
	delete u;
}

TEST_CASE("box centers its single child with padding; size/colors setters; get_text") {
	nwui *u = new nwui; nwui_init(u);
	char tb[8] = "v";
	nwui_node *inner = nwui_size(nwui_colors(nwui_label(u, "z"), 0x010203, 0x0a0b0c), 40, 20);
	nwui_node *bx = nwui_pad(nwui_box(u, inner), 8);
	nwui_set_root(u, bx);
	u->win_w = 80; u->win_h = 60;
	nwui_layout(u);
	CHECK(inner->x == 8);
	CHECK(inner->y == 8);
	CHECK(inner->w == 80 - 16);
	CHECK(inner->fg == 0x010203u);
	CHECK(inner->bg == 0x0a0b0cu);
	CHECK(inner->has_bg == 1);
	nwui_node *tf = nwui_textfield(u, tb, sizeof tb, 0, 0);
	CHECK(strcmp(nwui_get_text(tf), "v") == 0);   // textfield -> its buffer
	// an empty column measures to just its padding (no crash on zero children)
	nwui_node *empty = nwui_pad(nwui_column(u, (nwui_node *) 0), 5);
	nwui_measure(empty);
	CHECK(empty->mw == 10);
	CHECK(empty->mh == 10);
	delete u;
}

TEST_CASE("nwui_vbox/hbox/add build containers imperatively (FFI-friendly path)") {
	nwui *u = new nwui; nwui_init(u);
	nwui_node *col = nwui_vbox(u);
	CHECK(col->kind == NWUI_COLUMN);
	CHECK(nwui_add(col, nwui_label(u, "a")) == col);   // returns the parent
	nwui_add(col, nwui_label(u, "b"));
	CHECK(col->nchild == 2);
	nwui_node *row = nwui_hbox(u);
	CHECK(row->kind == NWUI_ROW);
	nwui_add(row, nwui_label(u, "x"));
	CHECK(row->nchild == 1);
	delete u;
}

TEST_CASE("CONFIGURE sets the window size + requests relayout; CLOSE stops the loop") {
	nwui *u = new nwui; nwui_init(u);
	nwui_set_root(u, nwui_label(u, "x"));
	nw_event e; memset(&e, 0, sizeof e);
	e.type = NW_EV_CONFIGURE; e.x = 320; e.y = 240;
	CHECK(nwui_dispatch(u, &e) == 1);
	CHECK(u->win_w == 320);
	CHECK(u->win_h == 240);
	CHECK(u->layout_dirty == 1);
	nw_event c; memset(&c, 0, sizeof c); c.type = NW_EV_CLOSE;
	CHECK(nwui_dispatch(u, &c) == 0);
	CHECK(u->closed == 1);
	delete u;
}
