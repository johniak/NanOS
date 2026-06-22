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
static void keyc(nwui *u, int code, char ch, int mods) {
	nw_event e; memset(&e, 0, sizeof e);
	e.type = NW_EV_KEY; e.down = 1; e.ch = ch; e.code = code; e.mods = mods;
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
	click(u, tf->x + NWUI_TF_PAD + 2 * 8, tf->y + 4);   // click past the end -> caret at 2
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

static void keycode(nwui* u, int code, char ch, int mods) {
	nw_event e; memset(&e, 0, sizeof e);
	e.type = NW_EV_KEY; e.down = 1; e.code = code; e.ch = ch; e.mods = mods;
	nwui_dispatch(u, &e);
}

TEST_CASE("textfield: mouse drag selects a range; typing replaces the selection") {
	nwui *u = new nwui; nwui_init(u);
	char tb[16] = "hello";
	nwui_node *tf = nwui_textfield(u, tb, sizeof tb, 0, 0);
	nwui_set_root(u, nwui_column(u, tf, (nwui_node*) 0));
	u->win_w = 200; u->win_h = 60; nwui_layout(u);
	int base = tf->x + NWUI_TF_PAD;
	pointer(u, base + 1 * 8, tf->y + 4, 1);        // press -> caret at char 1
	CHECK(tf->caret == 1); CHECK(tf->anchor == 1);
	pointer(u, base + 3 * 8, tf->y + 4, 1);        // drag (left held) -> select [1,3) = "el"
	CHECK(tf->caret == 3); CHECK(tf->anchor == 1);
	pointer(u, base + 3 * 8, tf->y + 4, 0);        // release
	key(u, 'X');                                    // replaces "el"
	CHECK(strcmp(tb, "hXlo") == 0);
	delete u;
}

TEST_CASE("textfield: shift+arrow extends selection, plain arrow collapses; backspace deletes selection") {
	nwui *u = new nwui; nwui_init(u);
	char tb[16] = "abc";
	nwui_node *tf = nwui_textfield(u, tb, sizeof tb, 0, 0);
	nwui_set_root(u, nwui_column(u, tf, (nwui_node*) 0));
	u->win_w = 200; u->win_h = 60; nwui_layout(u);
	pointer(u, tf->x + NWUI_TF_PAD, tf->y + 4, 1); pointer(u, tf->x + NWUI_TF_PAD, tf->y + 4, 0);
	CHECK(tf->caret == 0);
	keycode(u, NWUI_SC_RIGHT, 0, 1);               // shift+right -> sel [0,1)
	CHECK(tf->caret == 1); CHECK(tf->anchor == 0);
	keycode(u, NWUI_SC_RIGHT, 0, 1);               // -> sel [0,2)
	CHECK(tf->caret == 2);
	key(u, 8);                                      // backspace deletes the selection "ab"
	CHECK(strcmp(tb, "c") == 0);
	delete u;
}

TEST_CASE("Super+C copies the selection; Super+X cuts it") {
	nwui *u = new nwui; nwui_init(u);
	char tb[16] = "hello";
	nwui_node *tf = nwui_textfield(u, tb, sizeof tb, 0, 0);
	nwui_set_root(u, nwui_column(u, tf, (nwui_node*) 0));
	u->win_w = 200; u->win_h = 60; nwui_layout(u);
	int base = tf->x + NWUI_TF_PAD;
	pointer(u, base, tf->y + 4, 1); pointer(u, base + 2 * 8, tf->y + 4, 1); pointer(u, base + 2 * 8, tf->y + 4, 0);  // select "he"
	nw_event c; memset(&c, 0, sizeof c); c.type = NW_EV_COPY; c.cut = 0;
	nwui_dispatch(u, &c);
	CHECK(u->clip_set == 1);
	CHECK(u->clip_len == 2);
	CHECK(strncmp(u->clip_buf, "he", 2) == 0);
	// cut: copies + deletes
	c.cut = 1; nwui_dispatch(u, &c);
	CHECK(strcmp(tb, "llo") == 0);
	delete u;
}

TEST_CASE("right-click context menu: open, Select All, Paste sets clip_get, click closes") {
	nwui *u = new nwui; nwui_init(u);
	char tb[16] = "abcd";
	nwui_node *tf = nwui_textfield(u, tb, sizeof tb, 0, 0);
	nwui_set_root(u, nwui_column(u, tf, (nwui_node*) 0));
	u->win_w = 300; u->win_h = 200; nwui_layout(u);
	pointer(u, tf->x + 10, tf->y + 4, 2);          // right press -> menu opens
	CHECK(u->menu_open == 1);
	CHECK(u->menu_target == tf);
	pointer(u, tf->x + 10, tf->y + 4, 0);          // right release -> menu stays
	CHECK(u->menu_open == 1);
	int mx = u->menu_x + 5, my = u->menu_y + NWUI_MI_SELALL * NWUI_MENU_ITEM_H + 5;
	pointer(u, mx, my, 1); pointer(u, mx, my, 0);  // click "Select All"
	CHECK(u->menu_open == 0);
	CHECK(tf->anchor == 0); CHECK(tf->caret == 4);
	pointer(u, tf->x + 10, tf->y + 4, 2); pointer(u, tf->x + 10, tf->y + 4, 0);   // reopen
	int px = u->menu_x + 5, py = u->menu_y + NWUI_MI_PASTE * NWUI_MENU_ITEM_H + 5;
	pointer(u, px, py, 1); pointer(u, px, py, 0);  // click "Paste"
	CHECK(u->clip_get == 1);
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

static int g_activations;
static void on_activate(nwui_node *, void *u) { (*(int *) u)++; }

TEST_CASE("list: single click selects (no activate); double-click activates; out-of-range ignored") {
	nwui *u = new nwui; nwui_init(u);
	const char *items[] = { "alpha", "beta", "gamma", "delta" };
	g_activations = 0;
	nwui_node *L = nwui_list(u, on_activate, &g_activations);
	CHECK(L->kind == NWUI_LIST);
	CHECK(nwui_list_selected(L) == -1);              // nothing selected yet
	nwui_list_set(L, items, 4);
	nwui_set_root(u, nwui_flex(L, 1));
	u->win_w = 200; u->win_h = 100; nwui_layout(u);

	// single click row 2 (gamma): selects, but does NOT activate
	int ry = L->y + 1 + 2 * NWUI_ROW_H + NWUI_ROW_H / 2;
	u->now_ms = 1000;
	pointer(u, L->x + 5, ry, 1); pointer(u, L->x + 5, ry, 0);
	CHECK(nwui_list_selected(L) == 2);
	CHECK(g_activations == 0);                        // single click never opens
	CHECK(u->focus == L);

	// a second click on the same row within the double-click window -> activate
	u->now_ms = 1000 + NWUI_DBL_MS - 50;
	pointer(u, L->x + 5, ry, 1); pointer(u, L->x + 5, ry, 0);
	CHECK(g_activations == 1);

	// a click far below the last row selects nothing new
	pointer(u, L->x + 5, L->y + 1 + 99 * NWUI_ROW_H, 1);
	CHECK(nwui_list_selected(L) == 2);               // unchanged
	delete u;
}

TEST_CASE("list: two clicks too far apart in time are NOT a double-click") {
	nwui *u = new nwui; nwui_init(u);
	const char *items[] = { "a", "b", "c" };
	g_activations = 0;
	nwui_node *L = nwui_list(u, on_activate, &g_activations);
	nwui_list_set(L, items, 3);
	nwui_set_root(u, nwui_flex(L, 1));
	u->win_w = 200; u->win_h = 100; nwui_layout(u);
	int ry = L->y + 1 + NWUI_ROW_H / 2;
	u->now_ms = 5000;
	pointer(u, L->x + 5, ry, 1); pointer(u, L->x + 5, ry, 0);   // first click
	u->now_ms = 5000 + NWUI_DBL_MS + 100;                       // too slow
	pointer(u, L->x + 5, ry, 1); pointer(u, L->x + 5, ry, 0);   // second click
	CHECK(g_activations == 0);                                   // not a double-click
	CHECK(nwui_list_selected(L) == 0);                          // but still selected
	delete u;
}

TEST_CASE("list: Up/Down move the selection (clamped); Enter activates the selection") {
	nwui *u = new nwui; nwui_init(u);
	const char *items[] = { "one", "two", "three" };
	g_activations = 0;
	nwui_node *L = nwui_list(u, on_activate, &g_activations);
	nwui_list_set(L, items, 3);
	nwui_set_root(u, nwui_flex(L, 1));
	u->win_w = 200; u->win_h = 100; nwui_layout(u);
	int ry = L->y + 1 + NWUI_ROW_H / 2;
	pointer(u, L->x + 5, ry, 1); pointer(u, L->x + 5, ry, 0);   // select row 0, focus the list
	CHECK(nwui_list_selected(L) == 0);

	keycode(u, NWUI_SC_UP, 0, 0);                    // already at top -> clamp
	CHECK(nwui_list_selected(L) == 0);
	keycode(u, NWUI_SC_DOWN, 0, 0);
	CHECK(nwui_list_selected(L) == 1);
	keycode(u, NWUI_SC_DOWN, 0, 0);
	keycode(u, NWUI_SC_DOWN, 0, 0);                  // past the end -> clamp at 2
	CHECK(nwui_list_selected(L) == 2);

	int before = g_activations;
	key(u, '\n');                                    // Enter activates current selection
	CHECK(g_activations == before + 1);
	delete u;
}

TEST_CASE("list: selection scrolls into view when it leaves the visible window") {
	nwui *u = new nwui; nwui_init(u);
	const char *items[] = { "a", "b", "c", "d", "e", "f", "g", "h" };
	nwui_node *L = nwui_list(u, 0, 0);
	nwui_list_set(L, items, 8);
	nwui_set_root(u, nwui_flex(L, 1));
	// window tall enough for ~3 rows of list (after pad) -> scrolling required for 8 items
	u->win_w = 200; u->win_h = 3 * NWUI_ROW_H + 4; nwui_layout(u);
	int ry = L->y + 1 + NWUI_ROW_H / 2;
	pointer(u, L->x + 5, ry, 1); pointer(u, L->x + 5, ry, 0);   // select row 0
	CHECK(L->scroll == 0);
	int vis = L->h / NWUI_ROW_H;
	for (int i = 0; i < 7; i++) keycode(u, NWUI_SC_DOWN, 0, 0); // walk to the last row
	CHECK(nwui_list_selected(L) == 7);
	CHECK(L->scroll == 8 - vis);                     // bottom row scrolled into view
	delete u;
}

TEST_CASE("list scrollbar: clicking the track seeks the thumb to the cursor; drag fine-tunes") {
	nwui *u = new nwui; nwui_init(u);
	const char *items[30];
	for (int i = 0; i < 30; i++) items[i] = "row";
	nwui_node *L = nwui_list(u, 0, 0);
	nwui_list_set(L, items, 30);
	nwui_set_root(u, nwui_flex(L, 1));
	u->win_w = 200; u->win_h = 5 * NWUI_ROW_H; nwui_layout(u);   // ~5 rows visible, 30 items
	CHECK(L->scroll == 0);
	int vis = L->h / NWUI_ROW_H;
	int maxs = L->count - vis;
	CHECK(maxs > 0);                                             // overflow -> scrollbar present
	int sbx = L->x + L->w - NWUI_SB_W;

	// click low on the track (not on the thumb) -> the list seeks near the bottom + arms a drag
	pointer(u, sbx + 2, L->y + L->h - 2, 1);
	CHECK(L->sb_drag == 1);
	CHECK(L->scroll > maxs / 2);                                 // jumped toward the end, not paged
	// drag back up to the very top -> scroll 0
	pointer(u, sbx + 2, L->y, 1);
	CHECK(L->scroll == 0);
	pointer(u, sbx + 2, L->y, 0);                               // release
	CHECK(L->sb_drag == 0);

	// click low again (near the bottom edge) -> clamps to max
	pointer(u, sbx + 2, L->y + L->h - 1, 1);
	CHECK(L->scroll == maxs);
	pointer(u, sbx + 2, L->y + L->h - 1, 0);

	// a click in the CONTENT area (left of the scrollbar) selects a row, never scrolls
	int before = L->scroll;
	pointer(u, L->x + 4, L->y + 1 + NWUI_ROW_H / 2, 1); pointer(u, L->x + 4, L->y + 1 + NWUI_ROW_H / 2, 0);
	CHECK(L->scroll == before);                                 // unchanged
	CHECK(nwui_list_selected(L) == L->scroll);                  // selected the first visible row
	delete u;
}

TEST_CASE("list: nwui_list_set installs a fresh model — clears selection + scroll") {
	nwui *u = new nwui; nwui_init(u);
	const char *big[] = { "0", "1", "2", "3", "4" };
	nwui_node *L = nwui_list(u, 0, 0);
	nwui_list_set(L, big, 5);
	L->sel = 4; L->scroll = 3;
	const char *small[] = { "x", "y" };
	nwui_list_set(L, small, 2);                      // replace the model
	CHECK(L->count == 2);
	CHECK(nwui_list_selected(L) == -1);              // selection cleared (old rows are gone)
	CHECK(L->scroll == 0);                           // reset
	// the non-list guards return safe defaults
	nwui_node *lbl = nwui_label(u, "hi");
	CHECK(nwui_list_selected(lbl) == -1);
	nwui_list_set(lbl, small, 2);                    // no-op on a non-list (must not crash)
	delete u;
}

static int g_menu_fired;
static void on_menu_pick(nwui_node*, void* u) { *(int*) u = 1; }

TEST_CASE("nwui menu: encode builds the wire spec; dispatch invokes the right callback") {
	nwui *u = new nwui; nwui_init(u);
	int f = nwui_menu(u, "Files");
	nwui_menu_item(u, f, "New", 0, 0);
	nwui_menu_item(u, f, "Close", 0, 0);
	int g = nwui_menu(u, "Go");
	g_menu_fired = 0;
	nwui_menu_item(u, g, "Home", on_menu_pick, &g_menu_fired);
	CHECK(f == 0); CHECK(g == 1);

	char spec[128];
	int n = nwui_menu_encode(u, spec, sizeof spec);
	CHECK(n > 0);
	CHECK(strstr(spec, "Files") != 0);
	CHECK(strstr(spec, "Go") != 0);
	CHECK(strstr(spec, "Home") != 0);
	CHECK(spec[5] == 0x1f);          // "Files" then a field separator

	nwui_menu_dispatch(u, 1, 0);     // Go > Home -> fires the callback
	CHECK(g_menu_fired == 1);
	g_menu_fired = 0;
	nwui_menu_dispatch(u, 0, 0);     // Files > New (null cb) -> no crash
	nwui_menu_dispatch(u, 9, 9);     // out of range -> no-op
	CHECK(g_menu_fired == 0);
	delete u;
}

TEST_CASE("textarea inserts printable chars and newlines, backspace deletes") {
	nwui *u = new nwui; nwui_init(u);
	char tb[64] = "";
	nwui_node *ta = nwui_textarea(u, tb, sizeof tb, 0, 0);
	nwui_set_root(u, ta);
	u->win_w = 300; u->win_h = 200; nwui_layout(u);
	u->focus = ta; ta->focused = 1;          // textarea is focused

	key(u, 'h'); key(u, 'i'); keyc(u, 0x1C, '\n', 0); key(u, 'x');
	CHECK(strcmp(tb, "hi\nx") == 0);
	CHECK(ta->caret == 4);

	key(u, 8);                                // backspace
	CHECK(strcmp(tb, "hi\n") == 0);
	delete u;
}

TEST_CASE("textarea caret moves by line and reports line/col") {
	nwui *u = new nwui; nwui_init(u);
	char tb[64] = "ab\ncde\nf";
	nwui_node *ta = nwui_textarea(u, tb, sizeof tb, 0, 0);
	nwui_set_root(u, ta); u->win_w = 300; u->win_h = 200; nwui_layout(u);
	u->focus = ta; ta->focused = 1;
	ta->caret = 0; ta->anchor = 0;

	int ln, col;
	keyc(u, NWUI_SC_DOWN, 0, 0);              // into "cde", aim col 1
	nwui_textarea_caret(ta, &ln, &col); CHECK(ln == 2); CHECK(col == 1);
	keyc(u, NWUI_SC_END, 0, 0);
	nwui_textarea_caret(ta, &ln, &col); CHECK(ln == 2); CHECK(col == 4);  // after "cde"
	keyc(u, NWUI_SC_DOWN, 0, 0);              // "f" is shorter -> clamp to end
	nwui_textarea_caret(ta, &ln, &col); CHECK(ln == 3); CHECK(col == 2);
	keyc(u, NWUI_SC_HOME, 0, 0);
	nwui_textarea_caret(ta, &ln, &col); CHECK(ln == 3); CHECK(col == 1);
	delete u;
}

TEST_CASE("textarea delete key and page motion scroll the view") {
	nwui *u = new nwui; nwui_init(u);
	char tb[128] = "l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8";
	nwui_node *ta = nwui_textarea(u, tb, sizeof tb, 0, 0);
	nwui_set_root(u, ta); u->win_w = 200; u->win_h = 200; nwui_layout(u);
	u->focus = ta; ta->focused = 1; ta->caret = 0; ta->anchor = 0;
	ta->h = 4 * NW_FONT_H;                    // 4 visible rows

	keyc(u, NWUI_SC_DEL, 0, 0);               // deletes 'l' of l1
	CHECK(strncmp(tb, "1\n", 2) == 0);

	for (int i = 0; i < 6; i++) keyc(u, NWUI_SC_DOWN, 0, 0);  // caret onto a low line
	CHECK(ta->scroll > 0);                    // view scrolled to follow caret
	delete u;
}

TEST_CASE("textarea word-wrap multiplies visual rows") {
	nwui *u = new nwui; nwui_init(u);
	char tb[64] = "aaaaaaaaaa";               // 10 chars, no newline
	nwui_node *ta = nwui_textarea(u, tb, sizeof tb, 0, 0);
	nwui_set_root(u, ta); u->win_w = 200; u->win_h = 200; nwui_layout(u);
	ta->w = 5 * NW_FONT_W + 2 * 4;            // ~5 columns of text width
	nwui_textarea_set_wrap(ta, 1);
	CHECK(nwui_textarea_total_rows(ta) == 2); // 10 chars / 5 cols = 2 rows
	nwui_textarea_set_wrap(ta, 0);
	CHECK(nwui_textarea_total_rows(ta) == 1);
	delete u;
}

TEST_CASE("textarea click places caret, drag extends selection") {
	nwui *u = new nwui; nwui_init(u);
	char tb[64] = "abcd\nefgh";
	nwui_node *ta = nwui_textarea(u, tb, sizeof tb, 0, 0);
	nwui_set_root(u, ta); u->win_w = 300; u->win_h = 200; nwui_layout(u);
	int bx = ta->x + 4, by = ta->y + 2;
	pointer(u, bx + 2 * NW_FONT_W, by, 1);    // press over col 2 of row 0
	pointer(u, bx + 2 * NW_FONT_W, by, 0);
	CHECK(ta->caret == 2); CHECK(ta->anchor == 2);
	pointer(u, bx + 1 * NW_FONT_W, by, 1);    // press col 1
	pointer(u, bx + 3 * NW_FONT_W, by, 1);    // drag to col 3 (still pressed)
	CHECK(ta->anchor == 1); CHECK(ta->caret == 3);
	delete u;
}

TEST_CASE("textarea find, goto-line, select-all, insert-text") {
	nwui *u = new nwui; nwui_init(u);
	char tb[64] = "foo\nBar\nbar baz";
	nwui_node *ta = nwui_textarea(u, tb, sizeof tb, 0, 0);
	nwui_set_root(u, ta); u->win_w = 300; u->win_h = 200; nwui_layout(u);
	ta->caret = 0; ta->anchor = 0;

	CHECK(nwui_textarea_find(ta, "bar", 0, 0) == 1);   // case-insensitive: hits "Bar"
	CHECK(ta->anchor == 4); CHECK(ta->caret == 7);
	CHECK(nwui_textarea_find(ta, "bar", 1, 0) == 1);   // case-sensitive from caret: "bar baz"
	CHECK(ta->anchor == 8);
	CHECK(nwui_textarea_find(ta, "zzz", 0, 0) == 0);

	nwui_textarea_goto_line(ta, 2); CHECK(ta->caret == 4);
	nwui_textarea_select_all(ta); CHECK(ta->anchor == 0); CHECK(ta->caret == ta->tlen);

	ta->caret = 0; ta->anchor = 0;
	nwui_textarea_insert_text(ta, "X\nY");
	CHECK(strncmp(tb, "X\nYfoo", 6) == 0); CHECK(ta->caret == 3);
	delete u;
}

TEST_CASE("textarea copy/cut/paste via clipboard events") {
	nwui *u = new nwui; nwui_init(u);
	char tb[64] = "hello";
	nwui_node *ta = nwui_textarea(u, tb, sizeof tb, 0, 0);
	nwui_set_root(u, ta); u->win_w = 300; u->win_h = 200; nwui_layout(u);
	u->focus = ta; ta->focused = 1; ta->anchor = 0; ta->caret = 3;   // "hel" selected

	nw_event c; memset(&c, 0, sizeof c); c.type = NW_EV_COPY; c.cut = 1;
	nwui_dispatch(u, &c);
	CHECK(u->clip_set == 1); CHECK(u->clip_len == 3);
	CHECK(strncmp(u->clip_buf, "hel", 3) == 0);
	CHECK(strcmp(tb, "lo") == 0);                                     // cut removed "hel"

	nw_event p; memset(&p, 0, sizeof p); p.type = NW_EV_PASTE;
	p.text = "XY"; p.text_len = 2; ta->caret = 0; ta->anchor = 0;
	nwui_dispatch(u, &p);
	CHECK(strcmp(tb, "XYlo") == 0);
	delete u;
}
