/*
 * nwbench.c — GUI rendering benchmark for NanOS. A glass libnwui window that animates a
 * representative widget scene (three rows of buttons sweeping side-to-side + a widget-mix row)
 * by forcing a FULL relayout+repaint every frame — the exact worst-case interior path every
 * real app pays on scroll/typing — and reports frames per second + frame-time stats, both in
 * the window and on stdout (one line per second, for headless runs).
 *
 * Driven by nwui_pump() (the toolkit's non-blocking loop iteration) as fast as it will go:
 * FPS here = client paint + commit throughput of the libnw/libnwui engine, no pacing.
 *
 * AUTO MODE (for `make bench64-gl`): if $HOME/.nwbench-auto exists (seeded into the image by
 * the host, content = seconds to run, default 120), the flag is consumed (unlinked), the
 * benchmark runs unattended for that long, writes a parse-friendly report to
 * $HOME/nwbench-result.txt (fsync'd BEFORE the "nwbench: done" stdout marker, so the host can
 * quit QEMU on the marker and still read the file), and exits.
 */
#include "nwui.h"
#include "nwproto.h"   /* NW_STYLE_* */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ROWS     3     /* animated button rows */
#define COLS     4     /* buttons per row */
#define SWEEP    120   /* horizontal travel, px */
#define PERIODMS 1600  /* one full left-right-left sweep */
#define MAX_SECS 600   /* per-second series cap (auto mode) */
#define HBUCKETS 1024  /* frame-time histogram: 1 ms buckets, last = >=1023 ms overflow */

static int g_pause;

static nwui_node *g_fps;             /* the big FPS readout */
static nwui_node *g_stats;           /* avg/min/max frame time line */
static nwui_node *g_spacer[ROWS];    /* animated leading spacer of each row */
static char g_fps_text[48]   = "-- FPS";
static char g_stats_text[96] = "warming up...";
static char g_field[64]      = "type here while it runs";

/* auto mode state */
static int  g_auto_secs;             /* 0 = interactive (no flag file) */
static char g_home[128];

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

static void auto_init(void)
{
	const char *h = getenv("HOME");
	snprintf(g_home, sizeof g_home, "%s", (h && h[0]) ? h : "/disks/main/users/jan");
	char p[160];
	snprintf(p, sizeof p, "%s/.nwbench-auto", g_home);
	int fd = open(p, O_RDONLY);
	if (fd < 0)
		return;                              /* no flag: interactive */
	char buf[16];
	int n = (int) read(fd, buf, sizeof buf - 1);
	close(fd);
	unlink(p);                               /* consume: later launches are interactive again */
	g_auto_secs = 0;
	if (n > 0) { buf[n] = 0; g_auto_secs = atoi(buf); }
	if (g_auto_secs <= 0 || g_auto_secs > MAX_SECS) g_auto_secs = 120;
}

/* frame-time histogram (1 ms buckets) — the stutter metrics: percentiles + the 1%-low tail */
static long g_hist[HBUCKETS];

/* smallest frame-time t (ms) with count(<=t) >= q/1000 of all frames */
static long hist_percentile_ms(long frames, int q_x10)
{
	long need = (frames * q_x10 + 999) / 1000, cum = 0;
	for (int b = 0; b < HBUCKETS; b++) {
		cum += g_hist[b];
		if (cum >= need) return b;
	}
	return HBUCKETS - 1;
}

/* mean of the WORST 1% of frames (>=1 frame), in us — "1% low" fps = 1e6/this */
static long hist_low1pct_us(long frames)
{
	long take = frames / 100; if (take < 1) take = 1;
	long left = take, sum = 0;
	for (int b = HBUCKETS - 1; b >= 0 && left > 0; b--) {
		long n = g_hist[b] < left ? g_hist[b] : left;
		sum += n * ((long) b * 1000 + 500);   /* bucket midpoint, us */
		left -= n;
	}
	return sum / take;
}

static void write_result(long total_us, long frames, long gmin, long gmax,
                         const int *sec_fps, int nsec)
{
	static char out[8192];
	int len = 0;
	long avg   = frames ? total_us / frames : 0;
	long fps10 = total_us ? frames * 10000000L / total_us : 0;
	long p50 = hist_percentile_ms(frames, 500), p90 = hist_percentile_ms(frames, 900);
	long p99 = hist_percentile_ms(frames, 990);
	long low1 = frames ? hist_low1pct_us(frames) : 0;
	long low1fps10 = low1 ? 10000000L / low1 : 0;
	int  worst_sec = nsec ? sec_fps[0] : 0;
	for (int i = 1; i < nsec; i++)
		if (sec_fps[i] < worst_sec) worst_sec = sec_fps[i];
	len += snprintf(out + len, sizeof out - len,
	                "nwbench-result v1\nduration_us %ld\nframes %ld\nfps_x10 %ld\n"
	                "frame_avg_us %ld\nframe_min_us %ld\nframe_max_us %ld\n"
	                "frame_p50_ms %ld\nframe_p90_ms %ld\nframe_p99_ms %ld\n"
	                "low1pct_fps_x10 %ld\nworst_second_fps %d\nper_second_fps",
	                total_us, frames, fps10, avg, gmin, gmax,
	                p50, p90, p99, low1fps10, worst_sec);
	for (int i = 0; i < nsec && len < (int) sizeof out - 8; i++)
		len += snprintf(out + len, sizeof out - len, " %d", sec_fps[i]);
	len += snprintf(out + len, sizeof out - len, "\n");

	char p[160];
	snprintf(p, sizeof p, "%s/nwbench-result.txt", g_home);
	int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		write(fd, out, (unsigned) len);
		fsync(fd);                           /* on disk BEFORE the stdout marker below */
		close(fd);
	}
	printf("nwbench: done - %ld frames in %ld us (%ld.%ld fps, p99 %ld ms, 1%%low %ld.%ld fps), result -> %s\n",
	       frames, total_us, fps10 / 10, fps10 % 10, p99, low1fps10 / 10, low1fps10 % 10,
	       fd >= 0 ? p : "(write FAILED)");
}

int main(void)
{
	auto_init();
	nwui *u = nwui_open_style("GUI Bench", 560, 430, NW_STYLE_GLASS_CLIENT);
	if (!u)
		return 1;
	if (g_auto_secs)
		nwui_profile(u, 1);   /* headless runs also log the render/commit split per second */

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

	/* benchmark loop: animate -> full repaint -> commit, unpaced; 1 s reporting windows.
	 * Auto mode also accumulates run totals + the per-second series for the report. */
	static int sec_fps[MAX_SECS];
	int  nsec = 0;
	long bench_start = now_us();
	long win_start = bench_start, frame_start = bench_start;
	long sum_us = 0, min_us = 0, max_us = 0;
	long tot_frames = 0, tot_min = 0, tot_max = 0;
	int frames = 0;
	for (;;) {
		if (g_pause && !g_auto_secs) {         /* pause is an interactive-only affordance */
			if (!nwui_pump(u))                 /* keep events (incl. un-pause) flowing */
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
		tot_frames++;
		if (!tot_min || dt < tot_min) tot_min = dt;
		if (dt > tot_max) tot_max = dt;
		{
			long b = dt / 1000;
			g_hist[b < HBUCKETS ? b : HBUCKETS - 1]++;
		}

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
			if (nsec < MAX_SECS) sec_fps[nsec++] = frames;
			win_start = end; sum_us = min_us = max_us = 0; frames = 0;
		}
		if (g_auto_secs && end - bench_start >= (long) g_auto_secs * 1000000) {
			write_result(end - bench_start, tot_frames, tot_min, tot_max, sec_fps, nsec);
			break;
		}
	}
	return 0;
}
