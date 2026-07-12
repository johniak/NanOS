/*
 * nwbench.c — GUI rendering benchmark for NanOS. A glass libnwui window that animates a
 * representative widget scene (three rows of buttons sweeping side-to-side + a widget-mix row)
 * by forcing a FULL relayout+repaint every frame — the exact worst-case interior path every
 * real app pays on scroll/typing — and reports frames per second + frame-time stats, both in
 * the window and on stdout (one line per second, for headless runs).
 *
 * Driven by nwui_pump() (the toolkit's non-blocking loop iteration) as fast as it will go:
 * FPS here = client paint + commit throughput of the libnw/libnwui engine, no pacing.
 */
#include "nwui.h"
#include "nwproto.h"   /* NW_STYLE_* */
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define ROWS     3     /* animated button rows */
#define COLS     4     /* buttons per row */
#define SWEEP    120   /* horizontal travel, px */
#define PERIODMS 1600  /* one full left-right-left sweep */

static int g_pause;

static nwui_node *g_fps;             /* the big FPS readout */
static nwui_node *g_stats;           /* avg/min/max frame time line */
static nwui_node *g_spacer[ROWS];    /* animated leading spacer of each row */
static char g_fps_text[48]   = "-- FPS";
static char g_stats_text[96] = "warming up...";
static char g_field[64]      = "type here while it runs";

static long now_us(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (long) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Triangle wave 0..SWEEP with the given phase offset (integer math — no libm dependency;
 * a linear sweep reads just as well as a sine for judging smoothness). */
static int sweep_at(long ms, int phase_ms)
{
	long t = (ms + phase_ms) % (2 * PERIODMS);
	if (t < 0) t += 2 * PERIODMS;
	long x = t * SWEEP / PERIODMS;           /* 0..2*SWEEP */
	return (int) (x <= SWEEP ? x : 2 * SWEEP - x);
}

static void nop_cb(nwui_node *n, void *user) { (void) n; (void) user; }

int main(void)
{
	nwui *u = nwui_open_style("GUI Bench", 560, 430, NW_STYLE_GLASS_CLIENT);
	if (!u)
		return 1;

	/* header: FPS readout + stats */
	g_fps   = nwui_label(u, g_fps_text);
	g_stats = nwui_colors(nwui_label(u, g_stats_text), 0x9aa4b4, 0);

	/* animated rows: [animated spacer][buttons...] — resizing the spacer each frame shifts the
	 * whole row, so layout, every glass fill and every label repaint on every single frame */
	nwui_node *panel = nwui_panel(u, "Animated interior (full repaint per frame)");
	static char btxt[ROWS][COLS][8];
	for (int r = 0; r < ROWS; r++) {
		nwui_node *row = nwui_hbox(u);
		g_spacer[r] = nwui_size(nwui_box(u, (nwui_node *) 0), 1, 1);
		nwui_add(row, g_spacer[r]);
		for (int c = 0; c < COLS; c++) {
			snprintf(btxt[r][c], sizeof btxt[r][c], "B%d%d", r + 1, c + 1);
			nwui_add(row, nwui_button(u, btxt[r][c], nop_cb, 0));
		}
		nwui_add(panel, nwui_gap(row, 6));
	}

	/* widget-mix row: the other common scrim/well paints, so the scene resembles a real app */
	nwui_node *mix = nwui_hbox(u);
	nwui_add(mix, nwui_checkbox(u, "Pause", &g_pause, nop_cb, 0));
	nwui_add(mix, nwui_flex(nwui_textfield(u, g_field, sizeof g_field, nop_cb, 0), 1));
	nwui_gap(mix, 10);

	nwui_node *root = nwui_column(u,
		nwui_gap(g_fps, 2),
		g_stats,
		nwui_gap(panel, 4),
		mix,
		(nwui_node *) 0);
	nwui_set_root(u, nwui_pad(nwui_gap(root, 10), 14));

	/* benchmark loop: animate -> full repaint -> commit, unpaced; 1 s reporting windows */
	long win_start = now_us(), frame_start = win_start;
	long sum_us = 0, min_us = 0, max_us = 0;
	int frames = 0;
	for (;;) {
		if (g_pause) {
			if (!nwui_pump(u))                     /* keep events (incl. un-pause) flowing */
				break;
			usleep(30000);
			win_start = frame_start = now_us();    /* don't count the pause in the window */
			sum_us = min_us = max_us = 0; frames = 0;
			continue;
		}
		long ms = frame_start / 1000;
		for (int r = 0; r < ROWS; r++)             /* opposite-ish phases per row */
			nwui_size(g_spacer[r], 1 + sweep_at(ms, r * (2 * PERIODMS) / ROWS), 1);
		nwui_invalidate(u);
		if (!nwui_pump(u))
			break;
		long end = now_us(), dt = end - frame_start;
		frame_start = end;
		sum_us += dt; frames++;
		if (!min_us || dt < min_us) min_us = dt;
		if (dt > max_us) max_us = dt;

		if (end - win_start >= 1000000) {          /* close the 1 s window */
			long avg = frames ? sum_us / frames : 0;
			snprintf(g_fps_text, sizeof g_fps_text, "%d FPS", frames);
			snprintf(g_stats_text, sizeof g_stats_text,
			         "frame avg %ld.%01ld ms   min %ld.%01ld   max %ld.%01ld",
			         avg / 1000, (avg % 1000) / 100, min_us / 1000, (min_us % 1000) / 100,
			         max_us / 1000, (max_us % 1000) / 100);
			nwui_set_text(g_fps, g_fps_text);
			nwui_set_text(g_stats, g_stats_text);
			printf("nwbench: %d fps, frame avg %ld us min %ld max %ld\n",
			       frames, avg, min_us, max_us);
			win_start = end; sum_us = min_us = max_us = 0; frames = 0;
		}
	}
	return 0;
}
