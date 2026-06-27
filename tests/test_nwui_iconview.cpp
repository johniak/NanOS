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
    u->win_w = 300; u->win_h = 300;     // 300 / 92 -> 3 columns
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
    u->win_w = 300; u->win_h = 300;          // 3 columns
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
