/*
 * nwabout.c — "About This Computer" for NanoOS, launched from the logo menu. A small nwui window
 * showing REAL system info read from /proc (version, CPU, memory, uptime). Mirrors macOS's About
 * panel. Built on libnwui; the data comes from the shared sysinfo.h helper.
 */
#include "nwui.h"
#include "sysinfo.h"

static nwui_node *info_row(nwui *u, const char *key, const char *val)
{
	return nwui_gap(nwui_add(nwui_add(nwui_hbox(u),
	                                  nwui_flex(nwui_colors(nwui_label(u, key), 0x657184, 0), 1)),
	                         nwui_label(u, val)), 8);
}

int main(void)
{
	nwui *u = nwui_open("About This Computer", 360, 240);
	if (!u)
		return 1;

	char ver[128], cpu[80], mem[24], up[24];
	sysinfo_version(ver, sizeof ver);
	sysinfo_cpu(cpu, sizeof cpu);
	sysinfo_mem_str(mem, sizeof mem);
	sysinfo_uptime_str(up, sizeof up);

	/* a little gradient-ish logo tile + the name */
	nwui_node *logo = nwui_colors(nwui_size(nwui_box(u, (nwui_node *) 0), 56, 56), 0, 0x0012a8f4);

	nwui_node *rows = nwui_gap(nwui_pad(nwui_vbox(u), 12), 8);
	nwui_add(rows, info_row(u, "Processor", cpu));
	nwui_add(rows, info_row(u, "Memory",    mem));
	nwui_add(rows, info_row(u, "Uptime",    up));
	nwui_colors(rows, 0, 0x00ffffff);

	nwui_node *col = nwui_gap(nwui_pad(nwui_vbox(u), 20), 10);
	nwui_add(col, logo);
	nwui_add(col, nwui_label(u, "NanoOS"));
	nwui_add(col, nwui_colors(nwui_label(u, ver), 0x657184, 0));
	nwui_add(col, rows);

	nwui_set_root(u, col);
	nwui_run(u);
	return 0;
}
