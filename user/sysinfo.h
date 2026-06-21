/*
 * sysinfo.h — tiny header-only helpers that read real system info from /proc (the same files
 * `free`/`cat` read), for the Settings and About apps. Raw open/read + a small parser, no stdio
 * buffering on /proc. Header-only (static inline) so each app just #includes it.
 */
#ifndef NX_SYSINFO_H
#define NX_SYSINFO_H

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

/* Read the whole of `path` into buf[cap] (NUL-terminated). Returns bytes read (>=0). */
static int si_slurp(const char *path, char *buf, int cap)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) { buf[0] = 0; return 0; }
	int n = (int) read(fd, buf, cap - 1);
	close(fd);
	if (n < 0) n = 0;
	buf[n] = 0;
	return n;
}

/* Unsigned value following `key` in buf (e.g. "MemTotal:   131072 kB"), or 0. */
static unsigned si_num(const char *buf, const char *key)
{
	for (const char *p = buf; *p; p++) {
		const char *k = key, *q = p;
		while (*k && *q == *k) { q++; k++; }
		if (*k == 0) {
			while (*q == ' ' || *q == '\t' || *q == ':') q++;
			unsigned v = 0;
			while (*q >= '0' && *q <= '9') { v = v * 10 + (unsigned) (*q - '0'); q++; }
			return v;
		}
	}
	return 0;
}

/* Copy the text following `key` (after any " \t:") up to end-of-line into out[cap]. */
static void si_str(const char *buf, const char *key, char *out, int cap)
{
	out[0] = 0;
	for (const char *p = buf; *p; p++) {
		const char *k = key, *q = p;
		while (*k && *q == *k) { q++; k++; }
		if (*k == 0) {
			while (*q == ' ' || *q == '\t' || *q == ':') q++;
			int i = 0;
			while (*q && *q != '\n' && i < cap - 1) out[i++] = *q++;
			out[i] = 0;
			return;
		}
	}
}

/* /proc/version first line, trimmed of the trailing newline. */
static void sysinfo_version(char *out, int cap)
{
	char buf[256];
	si_slurp("/proc/version", buf, sizeof buf);
	int i = 0;
	for (; buf[i] && buf[i] != '\n' && i < cap - 1; i++) out[i] = buf[i];
	out[i] = 0;
}

/* /proc/cpuinfo "model name", or a fallback if the field is absent. */
static void sysinfo_cpu(char *out, int cap)
{
	char buf[1024];
	si_slurp("/proc/cpuinfo", buf, sizeof buf);
	si_str(buf, "model name", out, cap);
	if (!out[0]) si_str(buf, "vendor_id", out, cap);
	if (!out[0]) { const char *f = "x86 CPU"; int i = 0; for (; f[i] && i < cap - 1; i++) out[i] = f[i]; out[i] = 0; }
}

/* MemTotal in kB from /proc/meminfo. */
static unsigned sysinfo_mem_kb(void)
{
	char buf[512];
	si_slurp("/proc/meminfo", buf, sizeof buf);
	return si_num(buf, "MemTotal:");
}

/* Seconds since boot from /proc/uptime ("uptime: N s (...)"). */
static unsigned sysinfo_uptime_s(void)
{
	char buf[128];
	si_slurp("/proc/uptime", buf, sizeof buf);
	return si_num(buf, "uptime:");
}

/* Format MemTotal as a human string, e.g. "128 MB" / "16 GB". */
static void sysinfo_mem_str(char *out, int cap)
{
	unsigned kb = sysinfo_mem_kb();
	if (kb >= 1024u * 1024u) snprintf(out, cap, "%u GB", (kb + 512u * 1024u) / (1024u * 1024u));
	else                     snprintf(out, cap, "%u MB", (kb + 512u) / 1024u);
}

/* Format uptime as "Hh Mm Ss" / "Mm Ss" / "Ss". */
static void sysinfo_uptime_str(char *out, int cap)
{
	unsigned s = sysinfo_uptime_s(), h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
	if (h)      snprintf(out, cap, "%uh %um %us", h, m, sec);
	else if (m) snprintf(out, cap, "%um %us", m, sec);
	else        snprintf(out, cap, "%us", sec);
}

/* Framebuffer resolution from /dev/fb0 via the fbdev FBIOGET_VSCREENINFO ioctl (the same call
 * fbtest uses). fb_var_screeninfo starts with uint32 xres, yres, xres_virtual, yres_virtual,
 * xoffset, yoffset, bits_per_pixel — so indices 0, 1 and 6 give what About shows. Formats e.g.
 * "1024x768 @ 32-bit"; "no framebuffer" when there is none (VGA-text boot). */
static inline void sysinfo_display_str(char *out, int cap)
{
	extern int ioctl(int fd, unsigned long request, ...);
	out[0] = 0;
	int fd = open("/dev/fb0", O_RDONLY);
	if (fd < 0) { snprintf(out, cap, "no framebuffer"); return; }
	unsigned var[40];
	for (int i = 0; i < 40; i++) var[i] = 0;
	int rc = ioctl(fd, 0x4600 /*FBIOGET_VSCREENINFO*/, var);
	close(fd);
	if (rc < 0 || var[0] == 0) { snprintf(out, cap, "unknown"); return; }
	snprintf(out, cap, "%ux%u @ %u-bit", var[0], var[1], var[6]);
}

#endif /* NX_SYSINFO_H */
