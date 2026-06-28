#include "doctest.h"
#include "nwui_core.h"
#include "nwui.h"
#include "libnw.h"
#include <cstring>

static int g_act, g_chg;
static void on_act(nwui_node *, void *u) { (*(int *) u)++; }
static void on_chg(nwui_node *, void *) { g_chg++; }

static void pointer(nwui *u, int x, int y, int b) {
    nw_event e; memset(&e, 0, sizeof e);
    e.type = NW_EV_POINTER; e.x = x; e.y = y; e.buttons = b;
    nwui_dispatch(u, &e);
}
static void click(nwui *u, int x, int y) { pointer(u, x, y, 1); pointer(u, x, y, 0); }
static void click_mod(nwui *u, int x, int y, int mods) {
    nw_event e; memset(&e, 0, sizeof e);
    e.type = NW_EV_POINTER; e.x = x; e.y = y; e.buttons = 1; e.mods = mods; nwui_dispatch(u, &e);
    e.buttons = 0; nwui_dispatch(u, &e);
}
static int cell_x(nwui_node *iv, int idx) { return iv->x + (idx % iv->cols) * NWUI_ICON_CELL_W + NWUI_ICON_CELL_W / 2; }
static int cell_y(nwui_node *iv, int idx) { return iv->y + (idx / iv->cols) * NWUI_ICON_CELL_H - iv->scroll + NWUI_ICON_CELL_H / 2; }

static uint32_t dummy[4] = { 0, 0, 0, 0 };

static nwui_node *make_view(nwui *u, nwui_icon_item *items, int n) {
    for (int i = 0; i < n; i++) { items[i].label = "x"; items[i].icon = dummy; items[i].iw = 2; items[i].ih = 2; }
    nwui_node *iv = nwui_iconview(u, on_act, on_chg, &g_act);
    nwui_iconview_set(iv, items, n);
    nwui_set_root(u, iv);
    return iv;
}

TEST_CASE("iconview computes columns from width and selects the clicked cell") {
    nwui *u = new nwui; nwui_init(u);
    g_act = 0; g_chg = 0;
    static nwui_icon_item items[5];
    nwui_node *iv = make_view(u, items, 5);
    u->win_w = 380; u->win_h = 360;     // 380 / 120 -> 3 columns
    nwui_layout(u);
    CHECK(iv->cols == 3);
    CHECK(nwui_iconview_selected(iv) == -1);

    // click the cell at row 0, col 1 -> index 1
    int cx = iv->x + NWUI_ICON_CELL_W + NWUI_ICON_CELL_W / 2;
    int cy = iv->y + NWUI_ICON_CELL_H / 2;
    click(u, cx, cy);
    CHECK(nwui_iconview_selected(iv) == 1);
    CHECK(g_chg == 1);          // single click => on_change
    CHECK(g_act == 0);          // not a double click
    delete u;
}

TEST_CASE("iconview double-click on the same cell activates") {
    nwui *u = new nwui; nwui_init(u);
    g_act = 0; g_chg = 0;
    static nwui_icon_item items[2];
    nwui_node *iv = make_view(u, items, 2);
    u->win_w = 300; u->win_h = 300;
    nwui_layout(u);
    int cx = iv->x + NWUI_ICON_CELL_W / 2, cy = iv->y + NWUI_ICON_CELL_H / 2;
    u->now_ms = 1000; click(u, cx, cy);
    u->now_ms = 1100; click(u, cx, cy);     // within NWUI_DBL_MS of the first
    CHECK(g_act == 1);
    delete u;
}

TEST_CASE("iconview arrow keys move the selection by one (right) and by a row (down)") {
    nwui *u = new nwui; nwui_init(u);
    g_act = 0; g_chg = 0;
    static nwui_icon_item items[6];
    nwui_node *iv = make_view(u, items, 6);
    u->win_w = 380; u->win_h = 360;          // 3 columns
    nwui_layout(u);
    nwui_focus(u, iv);
    // select index 0 by clicking it
    click(u, iv->x + NWUI_ICON_CELL_W / 2, iv->y + NWUI_ICON_CELL_H / 2);
    CHECK(nwui_iconview_selected(iv) == 0);

    nw_event e; memset(&e, 0, sizeof e);
    e.type = NW_EV_KEY; e.down = 1; e.code = NWUI_SC_RIGHT;
    nwui_dispatch(u, &e);
    CHECK(nwui_iconview_selected(iv) == 1);

    memset(&e, 0, sizeof e);
    e.type = NW_EV_KEY; e.down = 1; e.code = NWUI_SC_DOWN;   // down a full row (cols=3)
    nwui_dispatch(u, &e);
    CHECK(nwui_iconview_selected(iv) == 4);
    delete u;
}

/* ---- multi-selection (Shift / Cmd click, keyboard extend, select-all) ---- */
TEST_CASE("iconview Cmd+click toggles cells into/out of the selection") {
    nwui *u = new nwui; nwui_init(u);
    static nwui_icon_item items[6];
    nwui_node *iv = make_view(u, items, 6);
    u->win_w = 380; u->win_h = 360; nwui_layout(u);      // 3 cols
    click(u, cell_x(iv, 0), cell_y(iv, 0));               // plain click -> just cell 0
    CHECK(nwui_iconview_selection_count(iv) == 1);
    CHECK(nwui_iconview_is_selected(iv, 0));
    click_mod(u, cell_x(iv, 2), cell_y(iv, 2), 2);        // Cmd+click cell 2 -> {0,2}
    CHECK(nwui_iconview_selection_count(iv) == 2);
    CHECK(nwui_iconview_is_selected(iv, 0));
    CHECK(nwui_iconview_is_selected(iv, 2));
    CHECK(nwui_iconview_selected(iv) == 2);              // lead is the last-clicked
    click_mod(u, cell_x(iv, 0), cell_y(iv, 0), 2);        // Cmd+click cell 0 again -> toggles off
    CHECK(nwui_iconview_selection_count(iv) == 1);
    CHECK(!nwui_iconview_is_selected(iv, 0));
    delete u;
}

TEST_CASE("iconview Shift+click selects the contiguous range from the anchor") {
    nwui *u = new nwui; nwui_init(u);
    static nwui_icon_item items[6];
    nwui_node *iv = make_view(u, items, 6);
    u->win_w = 380; u->win_h = 360; nwui_layout(u);
    click(u, cell_x(iv, 1), cell_y(iv, 1));               // anchor at 1
    click_mod(u, cell_x(iv, 4), cell_y(iv, 4), 1);        // Shift+click 4 -> {1,2,3,4}
    CHECK(nwui_iconview_selection_count(iv) == 4);
    for (int i = 1; i <= 4; i++) CHECK(nwui_iconview_is_selected(iv, i));
    CHECK(!nwui_iconview_is_selected(iv, 0));
    CHECK(!nwui_iconview_is_selected(iv, 5));
    // a plain click collapses back to one
    click(u, cell_x(iv, 5), cell_y(iv, 5));
    CHECK(nwui_iconview_selection_count(iv) == 1);
    CHECK(nwui_iconview_is_selected(iv, 5));
    delete u;
}

TEST_CASE("iconview Shift+arrow extends the selection; select-all / clear") {
    nwui *u = new nwui; nwui_init(u);
    static nwui_icon_item items[6];
    nwui_node *iv = make_view(u, items, 6);
    u->win_w = 380; u->win_h = 360; nwui_layout(u);
    nwui_focus(u, iv);
    click(u, cell_x(iv, 0), cell_y(iv, 0));
    nw_event e; memset(&e, 0, sizeof e); e.type = NW_EV_KEY; e.down = 1; e.code = NWUI_SC_RIGHT; e.mods = 1;
    nwui_dispatch(u, &e);                                 // Shift+Right -> {0,1}
    CHECK(nwui_iconview_selection_count(iv) == 2);
    CHECK(nwui_iconview_is_selected(iv, 1));
    nwui_iconview_select_all(iv);
    CHECK(nwui_iconview_selection_count(iv) == 6);
    nwui_iconview_clear_selection(iv);
    CHECK(nwui_iconview_selection_count(iv) == 0);
    CHECK(nwui_iconview_selected(iv) == -1);
    delete u;
}

TEST_CASE("iconview click on the empty area clears the selection") {
    nwui *u = new nwui; nwui_init(u);
    static nwui_icon_item items[5];
    nwui_node *iv = make_view(u, items, 5);
    u->win_w = 380; u->win_h = 360; nwui_layout(u);      // 3 cols, so index 5 (row1 col2) is empty
    click(u, cell_x(iv, 0), cell_y(iv, 0));
    CHECK(nwui_iconview_selection_count(iv) == 1);
    click(u, cell_x(iv, 5), cell_y(iv, 5));              // empty cell slot -> clears
    CHECK(nwui_iconview_selection_count(iv) == 0);
    CHECK(nwui_iconview_selected(iv) == -1);
    delete u;
}

/* ---- smooth (pixel) scrolling ---- */
TEST_CASE("iconview scrolls by pixels (smooth), not whole rows, and clamps") {
    nwui *u = new nwui; nwui_init(u);
    static nwui_icon_item items[12];
    nwui_node *iv = make_view(u, items, 12);
    u->win_w = 380; u->win_h = 150; nwui_layout(u);      // 3 cols -> 4 rows, content 432 > 150
    CHECK(iv->scroll == 0);
    nw_event e; memset(&e, 0, sizeof e);
    e.type = NW_EV_POINTER; e.x = iv->x + 10; e.y = iv->y + 10; e.wheel = -1;   // wheel down
    nwui_dispatch(u, &e);
    CHECK(iv->scroll == NWUI_ICON_CELL_H / 3);           // a fraction of a row, not a full row
    // a big scroll clamps to (content_h - h) = 4*108 - 150 = 282
    memset(&e, 0, sizeof e); e.type = NW_EV_POINTER; e.x = iv->x + 10; e.y = iv->y + 10; e.wheel = -100;
    nwui_dispatch(u, &e);
    CHECK(iv->scroll == 4 * NWUI_ICON_CELL_H - 150);
    delete u;
}

/* ---- drag-and-drop ---- */
static int g_drag, g_drop, g_drop_cell;
static char g_drop_text[128];
static void on_drag_cb(nwui_node *n, void *) { g_drag++; nwui_begin_drag(n->owner, "src/path"); }
static void on_drop_cb(nwui_node *n, void *) {
    g_drop++;
    g_drop_cell = nwui_iconview_drop_cell(n);
    const char *t = nwui_iconview_drop_text(n);
    g_drop_text[0] = 0;
    if (t) { int i = 0; for (; t[i] && i < 127; i++) g_drop_text[i] = t[i]; g_drop_text[i] = 0; }
}

TEST_CASE("iconview drag gesture: press + move past the threshold fires on_drag -> begin_drag") {
    nwui *u = new nwui; nwui_init(u);
    static nwui_icon_item items[3];
    nwui_node *iv = make_view(u, items, 3);
    nwui_iconview_set_dnd(iv, on_drag_cb, on_drop_cb);
    u->win_w = 400; u->win_h = 300; nwui_layout(u);
    g_drag = 0;
    int cx = iv->x + NWUI_ICON_CELL_W / 2, cy = iv->y + NWUI_ICON_CELL_H / 2;
    pointer(u, cx, cy, 1);                // press on cell 0 -> a drag candidate
    CHECK(u->drag_cand == iv);
    pointer(u, cx + 12, cy + 3, 1);       // move >5px while held -> on_drag fires once
    CHECK(g_drag == 1);
    CHECK(u->drag_req == 1);              // on_drag called nwui_begin_drag
    CHECK(u->drag_cand == nullptr);       // candidate consumed (fires only once)
    delete u;
}

TEST_CASE("iconview drag motion highlights the hovered cell, leave clears it") {
    nwui *u = new nwui; nwui_init(u);
    static nwui_icon_item items[4];
    nwui_node *iv = make_view(u, items, 4);
    nwui_iconview_set_dnd(iv, on_drag_cb, on_drop_cb);
    u->win_w = 400; u->win_h = 300; nwui_layout(u);
    int dx = iv->x + 2 * NWUI_ICON_CELL_W + NWUI_ICON_CELL_W / 2;   // cell index 2
    int dy = iv->y + NWUI_ICON_CELL_H / 2;
    nw_event e; memset(&e, 0, sizeof e); e.type = NW_EV_DRAG_MOTION; e.x = dx; e.y = dy;
    nwui_dispatch(u, &e);
    CHECK(iv->drop_hover == 2);
    nw_event l; memset(&l, 0, sizeof l); l.type = NW_EV_DRAG_LEAVE;
    nwui_dispatch(u, &l);
    CHECK(iv->drop_hover == -1);
    delete u;
}

TEST_CASE("iconview drop: NW_EV_DROP fires on_drop with the target cell + payload") {
    nwui *u = new nwui; nwui_init(u);
    static nwui_icon_item items[4];
    nwui_node *iv = make_view(u, items, 4);
    nwui_iconview_set_dnd(iv, on_drag_cb, on_drop_cb);
    u->win_w = 400; u->win_h = 300; nwui_layout(u);
    g_drop = 0; g_drop_cell = -99;
    int dx = iv->x + NWUI_ICON_CELL_W + NWUI_ICON_CELL_W / 2;       // cell index 1
    int dy = iv->y + NWUI_ICON_CELL_H / 2;
    nw_event e; memset(&e, 0, sizeof e);
    e.type = NW_EV_DROP; e.x = dx; e.y = dy;
    e.text = "/disks/main/x.txt"; e.text_len = (int) strlen(e.text);
    nwui_dispatch(u, &e);
    CHECK(g_drop == 1);
    CHECK(g_drop_cell == 1);
    CHECK(strcmp(g_drop_text, "/disks/main/x.txt") == 0);
    delete u;
}

/* ---- iconview clipboard (Cmd+C/X/V delivered as COPY/PASTE events) ---- */
static int g_cv_copy, g_cv_paste, g_cv_cut;
static void on_copy_cb(nwui_node *n, void *) { g_cv_copy++; g_cv_cut = nwui_iconview_copy_cut(n); }
static void on_paste_cb(nwui_node *, void *) { g_cv_paste++; }

TEST_CASE("iconview clipboard: COPY/CUT/PASTE events fire on_copy/on_paste") {
    nwui *u = new nwui; nwui_init(u);
    static nwui_icon_item items[3];
    nwui_node *iv = make_view(u, items, 3);
    nwui_iconview_set_clipboard(iv, on_copy_cb, on_paste_cb);
    u->win_w = 400; u->win_h = 300; nwui_layout(u);
    u->focus = iv; iv->focused = 1;
    g_cv_copy = g_cv_paste = 0; g_cv_cut = -1;
    nw_event c; memset(&c, 0, sizeof c); c.type = NW_EV_COPY; c.cut = 0;   // Cmd+C
    nwui_dispatch(u, &c);
    CHECK(g_cv_copy == 1);
    CHECK(g_cv_cut == 0);
    nw_event x; memset(&x, 0, sizeof x); x.type = NW_EV_COPY; x.cut = 1;   // Cmd+X
    nwui_dispatch(u, &x);
    CHECK(g_cv_copy == 2);
    CHECK(g_cv_cut == 1);
    nw_event v; memset(&v, 0, sizeof v); v.type = NW_EV_PASTE;            // Cmd+V
    nwui_dispatch(u, &v);
    CHECK(g_cv_paste == 1);
    delete u;
}
