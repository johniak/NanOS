/*
 * nwabout.c — "About This Computer" for NanoOS, launched from the logo menu. A small nwui window
 * showing REAL system info read from /proc (version, CPU, memory, uptime). Mirrors macOS's About
 * panel. Built on libnwui; the data comes from the shared sysinfo.h helper.
 */
#include "nwui.h"
#include "sysinfo.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

/* Load the branded logo (flat 32bpp: [u32 w][u32 h][w*h pixels], from `make assets`) for the
 * NWUI_IMAGE tile. Returns 1 + dims on success; About falls back to a plain tile otherwise. */
static uint32_t g_logo[96 * 96];
static int load_logo(int *w, int *h)
{
	int fd = open("/disks/main/nanos/share/logo.raw", O_RDONLY);
	if (fd < 0) return 0;
	uint32_t hdr[2]; int ok = 0;
	if (read(fd, hdr, sizeof hdr) == (int) sizeof hdr &&
	    hdr[0] >= 1 && hdr[0] <= 96 && hdr[1] >= 1 && hdr[1] <= 96) {
		unsigned need = hdr[0] * hdr[1] * 4, got = 0; char *p = (char *) g_logo;
		for (;;) { int n = read(fd, p + got, need - got); if (n <= 0) break; got += n; if (got >= need) break; }
		if (got == need) { *w = (int) hdr[0]; *h = (int) hdr[1]; ok = 1; }
	}
	close(fd);
	return ok;
}

static nwui_node *info_row(nwui *u, const char *key, const char *val)
{
	return nwui_gap(nwui_add(nwui_add(nwui_hbox(u),
	                                  nwui_flex(nwui_colors(nwui_label(u, key), 0x657184, 0), 1)),
	                         nwui_label(u, val)), 8);
}

int main(void)
{
	nwui *u = nwui_open("About This Computer", 360, 270);
	if (!u)
		return 1;

	char ver[128], cpu[80], mem[24], up[24], disp[32];
	sysinfo_version(ver, sizeof ver);
	sysinfo_cpu(cpu, sizeof cpu);
	sysinfo_mem_str(mem, sizeof mem);
	sysinfo_uptime_str(up, sizeof up);
	sysinfo_display_str(disp, sizeof disp);

	/* the branded NanOS logo (falls back to a plain tile if the artwork is not installed) */
	int lw = 0, lh = 0;
	nwui_node *logo = load_logo(&lw, &lh)
	                ? nwui_image(u, g_logo, lw, lh)
	                : nwui_colors(nwui_size(nwui_box(u, (nwui_node *) 0), 56, 56), 0, 0x0012a8f4);

	nwui_node *rows = nwui_gap(nwui_pad(nwui_vbox(u), 12), 8);
	nwui_add(rows, info_row(u, "Processor", cpu));
	nwui_add(rows, info_row(u, "Memory",    mem));
	nwui_add(rows, info_row(u, "Display",   disp));
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
