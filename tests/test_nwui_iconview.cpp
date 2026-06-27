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
