/*
 * nwset.c — NanoOS "Settings" demo app, built on libnwui. A sidebar of categories next to a
 * panel with the system-info rows, in the NanoOS visual language. Static content (a demo), but
 * exercises the toolkit: nested row/column layout, flex, a coloured "card" container, and the
 * selected-row accent. The whole look comes from the toolkit + compositor.
 */
#include "nwui.h"
#include "sysinfo.h"
#include <unistd.h>

static void m_close(nwui_node *self, void *u) { (void) self; (void) u; _exit(0); }

/* One "Key . . . . Value" row: the key label grows (flex) so the value sits at the right. */
static nwui_node *info_row(nwui *u, const char *key, const char *val)
{
	return nwui_gap(nwui_add(nwui_add(nwui_hbox(u),
	                                  nwui_flex(nwui_label(u, key), 1)),
	                         nwui_colors(nwui_label(u, val), 0x657184, 0)), 8);
}

static nwui_node *nav(nwui *u, const char *text, int sel)
{
	nwui_node *l = nwui_label(u, text);
	if (sel) nwui_colors(l, 0x075da0, 0);
	return nwui_pad(l, 4);
}

int main(void)
{
	nwui *u = nwui_open("Settings", 470, 340);
	if (!u)
		return 1;
	int ms = nwui_menu(u, "Settings"); nwui_menu_item(u, ms, "Close", m_close, 0);

	nwui_node *side = nwui_gap(nwui_pad(nwui_vbox(u), 12), 6);
	nwui_add(side, nav(u, "System", 1));
	nwui_add(side, nav(u, "Network", 0));
	nwui_add(side, nav(u, "Display", 0));
	nwui_add(side, nav(u, "Privacy", 0));
	nwui_add(side, nav(u, "Accounts", 0));
	nwui_colors(side, 0, 0x00eef3f9); nwui_size(side, 132, 0);

	char ver[128], cpu[80], mem[24], up[24];      /* real values from /proc */
	sysinfo_version(ver, sizeof ver);
	sysinfo_cpu(cpu, sizeof cpu);
	sysinfo_mem_str(mem, sizeof mem);
	sysinfo_uptime_str(up, sizeof up);

	nwui_node *card = nwui_gap(nwui_pad(nwui_vbox(u), 14), 9);
	nwui_add(card, info_row(u, "Device name",    "NanoBook"));
	nwui_add(card, info_row(u, "Processor",      cpu));
	nwui_add(card, info_row(u, "Memory",         mem));
	nwui_add(card, info_row(u, "Uptime",         up));
	nwui_add(card, info_row(u, "System version", ver));
	nwui_colors(card, 0, 0x00ffffff);

	nwui_node *main_col = nwui_gap(nwui_pad(nwui_vbox(u), 18), 10);
	nwui_add(main_col, nwui_label(u, "NanoOS"));
	nwui_add(main_col, nwui_colors(nwui_label(u, "System Settings"), 0x657184, 0));
	nwui_add(main_col, card);

	nwui_node *root = nwui_hbox(u);
	nwui_add(root, side);
	nwui_add(root, nwui_flex(main_col, 1));
	nwui_set_root(u, root);

	nwui_run(u);
	return 0;
}
