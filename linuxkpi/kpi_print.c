/*
 * linuxkpi/kpi_print.c — a self-contained vsnprintf subset + printk for the LinuxKPI shim.
 *
 * Supports: %d %i %u %x %X %o %c %s %% with flags ('-' left, '0' zero-pad, '+', ' '),
 * field width (number or '*'), precision (.number, for strings = max len), and length
 * modifiers h/hh/l/ll/z/t. Pointers: %p / %px / %pK print the raw value in hex (we do not
 * hash — this is a bring-up log); %pa prints the dereferenced phys/dma address in hex.
 *
 * vscnprintf returns the number of chars actually written (clamped, NUL-terminated);
 * snprintf/vsnprintf return the would-be length (C/Linux semantics).
 */
#include <linux/printk.h>
#include <linux/string.h>
#include "lkpi_knx.h"

struct out {
	char *buf;
	size_t size;   /* capacity incl NUL */
	size_t total;  /* would-be length (not clamped) */
};

static void emit(struct out *o, char c) {
	if (o->total + 1 < o->size)
		o->buf[o->total] = c;
	o->total++;
}

static void emit_str(struct out *o, const char *s, int prec) {
	int n = 0;
	if (!s) s = "(null)";
	while (s[n] && (prec < 0 || n < prec)) {
		emit(o, s[n]);
		n++;
	}
}

/* Render an unsigned value in the given base; digits high-or-low case. */
static int utoa(unsigned long long v, unsigned base, int upper, char *tmp) {
	const char *lo = "0123456789abcdef";
	const char *hi = "0123456789ABCDEF";
	const char *d = upper ? hi : lo;
	int n = 0;
	if (v == 0)
		tmp[n++] = '0';
	while (v) {
		tmp[n++] = d[v % base];
		v /= base;
	}
	return n; /* digits are in reverse order in tmp */
}

static void emit_num(struct out *o, unsigned long long v, unsigned base, int upper,
                     int neg, int width, int prec, int left, int zero, int plus, int space) {
	char tmp[32];
	int ndig = utoa(v, base, upper, tmp);
	int nzero = (prec > ndig) ? (prec - ndig) : 0;  /* precision = min digits */
	int sign = neg ? 1 : (plus ? 1 : (space ? 1 : 0));
	char signc = neg ? '-' : (plus ? '+' : ' ');
	int body = ndig + nzero + sign;
	int pad = (width > body) ? (width - body) : 0;

	if (prec >= 0) zero = 0;  /* precision overrides zero-padding */

	if (!left && !zero)
		for (int i = 0; i < pad; i++) emit(o, ' ');
	if (sign) emit(o, signc);
	if (!left && zero)
		for (int i = 0; i < pad; i++) emit(o, '0');
	for (int i = 0; i < nzero; i++) emit(o, '0');
	for (int i = ndig - 1; i >= 0; i--) emit(o, tmp[i]);
	if (left)
		for (int i = 0; i < pad; i++) emit(o, ' ');
}

/* Core engine: formats into buf (clamped to size), returns the WOULD-BE length. */
static int do_format(char *buf, size_t size, const char *fmt, va_list ap) {
	struct out o = { buf, size, 0 };
	for (const char *p = fmt; *p; p++) {
		if (*p != '%') {
			/* strip a leading KERN_SOH level marker only at string start */
			emit(&o, *p);
			continue;
		}
		p++;
		int left = 0, zero = 0, plus = 0, space = 0;
		for (;; p++) {
			if (*p == '-') left = 1;
			else if (*p == '0') zero = 1;
			else if (*p == '+') plus = 1;
			else if (*p == ' ') space = 1;
			else break;
		}
		int width = 0;
		if (*p == '*') { width = va_arg(ap, int); p++; if (width < 0) { left = 1; width = -width; } }
		else while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
		int prec = -1;
		if (*p == '.') {
			p++;
			prec = 0;
			if (*p == '*') { prec = va_arg(ap, int); p++; }
			else while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
		}
		int lng = 0; /* 1 = long, 2 = long long, 3 = size_t */
		for (;;) {
			if (*p == 'l') { lng = (lng == 1) ? 2 : 1; p++; }
			else if (*p == 'z' || *p == 't') { lng = 3; p++; }
			else if (*p == 'h') { p++; }
			else break;
		}
		char c = *p;
		switch (c) {
		case 'd': case 'i': {
			long long v = (lng >= 2) ? va_arg(ap, long long)
			            : (lng == 1) ? va_arg(ap, long)
			            : (lng == 3) ? (long long)va_arg(ap, size_t)
			                         : va_arg(ap, int);
			int neg = v < 0;
			unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1ull : (unsigned long long)v;
			emit_num(&o, u, 10, 0, neg, width, prec, left, zero, plus, space);
			break;
		}
		case 'u': case 'x': case 'X': case 'o': {
			unsigned long long v = (lng >= 2) ? va_arg(ap, unsigned long long)
			            : (lng == 1) ? va_arg(ap, unsigned long)
			            : (lng == 3) ? (unsigned long long)va_arg(ap, size_t)
			                         : va_arg(ap, unsigned int);
			unsigned base = (c == 'o') ? 8 : (c == 'u') ? 10 : 16;
			emit_num(&o, v, base, c == 'X', 0, width, prec, left, zero, plus, space);
			break;
		}
		case 'c': {
			char ch = (char)va_arg(ap, int);
			int pad = width > 1 ? width - 1 : 0;
			if (!left) for (int i = 0; i < pad; i++) emit(&o, ' ');
			emit(&o, ch);
			if (left) for (int i = 0; i < pad; i++) emit(&o, ' ');
			break;
		}
		case 's': {
			const char *s = va_arg(ap, const char *);
			const char *ss = s ? s : "(null)";
			int len = 0; while (ss[len] && (prec < 0 || len < prec)) len++;
			int pad = width > len ? width - len : 0;
			if (!left) for (int i = 0; i < pad; i++) emit(&o, ' ');
			emit_str(&o, s, prec);
			if (left) for (int i = 0; i < pad; i++) emit(&o, ' ');
			break;
		}
		case 'p': {
			char n = p[1];
			if (n == 'a') {       /* %pa: pointer to a phys_addr_t/dma_addr_t */
				p++;
				phys_addr_t *pa = va_arg(ap, phys_addr_t *);
				unsigned long long v = pa ? (unsigned long long)*pa : 0;
				emit_str(&o, "0x", -1);
				emit_num(&o, v, 16, 0, 0, 0, -1, 0, 0, 0, 0);
			} else if (n == 'V') { /* %pV: recursive struct va_format (DRM/dev_printk) */
				struct lkpi_va_format { const char *fmt; va_list *va; };
				struct lkpi_va_format *vaf = va_arg(ap, struct lkpi_va_format *);
				p++;
				if (vaf && vaf->fmt) {
					char tmp[512];
					va_list cp;
					__builtin_va_copy(cp, *vaf->va);
					do_format(tmp, sizeof(tmp), vaf->fmt, cp);
					va_end(cp);
					emit_str(&o, tmp, -1);
				}
			} else if (n == 's' || n == 'S' || n == 'f' || n == 'F' || n == 'B') {
				/* %ps/%pS/%pf/%pF/%pB: symbol name — no kallsyms in the shim, show hex */
				p++;
				unsigned long long v = (unsigned long long)(size_t)va_arg(ap, void *);
				emit_str(&o, "0x", -1);
				emit_num(&o, v, 16, 0, 0, 0, -1, 0, 0, 0, 0);
			} else if (n == 'e') { /* %pe: ERR_PTR -> signed errno */
				p++;
				long e = (long)(size_t)va_arg(ap, void *);
				emit_str(&o, "err:", -1);
				emit_num(&o, (unsigned long long)(e < 0 ? -e : e), 10, 0, 0, 0, -1, 0, 0, 0, 0);
			} else {              /* %p / %px / %pK: raw pointer hex */
				if (n == 'x' || n == 'K') p++;
				unsigned long long v = (unsigned long long)(size_t)va_arg(ap, void *);
				emit_str(&o, "0x", -1);
				emit_num(&o, v, 16, 0, 0, 0, -1, 0, 0, 0, 0);
			}
			break;
		}
		case '%':
			emit(&o, '%');
			break;
		case '\0':
			p--; /* trailing % */
			break;
		default:
			emit(&o, '%');
			emit(&o, c);
			break;
		}
	}
	if (o.size) {
		size_t end = (o.total < o.size) ? o.total : o.size - 1;
		o.buf[end] = '\0';
	}
	return (int)o.total;  /* would-be length (excl NUL) */
}

/* vscnprintf: chars actually written (clamped to size-1), NUL-terminated. */
int vscnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
	int would = do_format(buf, size, fmt, ap);
	if (size == 0) return 0;
	return (would < (int)size) ? would : (int)size - 1;
}

/* vsnprintf: would-be length (C/Linux semantics). */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
	return do_format(buf, size, fmt, ap);
}

/* scnprintf / vscnprintf are Linux-only names (no libc collision) — always defined. */
int scnprintf(char *buf, size_t size, const char *fmt, ...) {
	va_list ap; va_start(ap, fmt);
	int r = vscnprintf(buf, size, fmt, ap);
	va_end(ap);
	return r;
}

/* snprintf/sprintf/vsnprintf collide with libc in host-test builds; the kext (no libc)
 * needs them under these exact names because Linux source calls them. */
#ifndef NANOS_HOST_TEST
int snprintf(char *buf, size_t size, const char *fmt, ...) {
	va_list ap; va_start(ap, fmt);
	int r = do_format(buf, size, fmt, ap);
	va_end(ap);
	return r;
}

int sprintf(char *buf, const char *fmt, ...) {
	va_list ap; va_start(ap, fmt);
	int r = do_format(buf, (size_t)-1, fmt, ap);
	va_end(ap);
	return r;
}
#endif

int printk(const char *fmt, ...) {
	char line[512];
	va_list ap; va_start(ap, fmt);
	int r = vscnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	/* strip a leading KERN_SOH ("\001" + level digit) so logs read cleanly */
	const char *out = line;
	if (out[0] == '\001' && out[1]) out += 2;
	knx_log(out);
	return r;
}
