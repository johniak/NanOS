#include "doctest.h"
#include "nwui_core.h"
#include "nwui.h"
#include "libnw.h"

TEST_CASE("panel reserves a title header and stacks children beneath it") {
    nwui *u = new nwui; nwui_init(u);
    nwui_node *p = nwui_panel(u, "Other Places");
    nwui_node *a = nwui_label(u, "Home");
    nwui_node *b = nwui_label(u, "My Computer");
    nwui_add(p, a); nwui_add(p, b);
    nwui_set_root(u, p);
    u->win_w = 160; u->win_h = 200;
    nwui_layout(u);
    CHECK(a->y >= p->y + NWUI_PANEL_TITLE_H);   // first child below the header band
    CHECK(b->y >  a->y);                         // stacked vertically
    delete u;
}

static void on_click(nwui_node *, void *) {}

TEST_CASE("flat link is a button-kind row with link height; set_active toggles the pill flag") {
    nwui *u = new nwui; nwui_init(u);
    nwui_node *l = nwui_link(u, "Home", on_click, 0);
    CHECK(l->kind == NWUI_BUTTON);
    CHECK(l->flat == 1);
    CHECK(l->active == 0);
    nwui_measure(l);
    CHECK(l->mh == NWUI_LINK_H);
    nwui_link_set_active(l, 1);
    CHECK(l->active == 1);
    nwui_link_set_active(l, 0);
    CHECK(l->active == 0);
    delete u;
}

TEST_CASE("panel measures tall enough for its header plus children") {
    nwui *u = new nwui; nwui_init(u);
    nwui_node *p = nwui_panel(u, "Details");
    nwui_add(p, nwui_label(u, "name"));
    nwui_set_root(u, p);
    nwui_measure(p);
    CHECK(p->mh >= NWUI_PANEL_TITLE_H + NW_FONT_H);
    delete u;
}
