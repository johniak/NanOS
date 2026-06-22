#include "nw_settings.h"

void nw_settings_defaults(struct nw_settings *s)
{
	s->blur               = 0;    /* off: the backdrop blur is the expensive path */
	s->blur_level         = 60;
	s->transparency       = 1;    /* glass on */
	s->transparency_level = 50;
}

static int clamp100(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }

/* Length of the leading run of bytes equal to a space or tab. */
static int skip_ws(const char *p, int n)
{
	int i = 0;
	while (i < n && (p[i] == ' ' || p[i] == '\t')) i++;
	return i;
}

/* Case-insensitive compare of token[0,tn) against the NUL-terminated `word`. */
static int tok_eq(const char *tok, int tn, const char *word)
{
	int i = 0;
	for (; i < tn && word[i]; i++) {
		char a = tok[i], b = word[i];
		if (a >= 'A' && a <= 'Z') a = (char) (a - 'A' + 'a');
		if (a != b) return 0;
	}
	return i == tn && word[i] == 0;
}

static int parse_bool(const char *tok, int tn, int fallback)
{
	if (tok_eq(tok, tn, "true") || tok_eq(tok, tn, "on") ||
	    tok_eq(tok, tn, "yes")  || tok_eq(tok, tn, "1")) return 1;
	if (tok_eq(tok, tn, "false") || tok_eq(tok, tn, "off") ||
	    tok_eq(tok, tn, "no")    || tok_eq(tok, tn, "0")) return 0;
	return fallback;
}

static int parse_int(const char *tok, int tn)
{
	int v = 0, i = 0;
	while (i < tn && tok[i] >= '0' && tok[i] <= '9') { v = v * 10 + (tok[i] - '0'); i++; }
	return v;
}

void nw_settings_parse(const char *buf, int len, struct nw_settings *s)
{
	int i = 0;
	while (i < len) {
		int ls = i;                                  /* line start */
		while (i < len && buf[i] != '\n') i++;
		int le = i;                                  /* line end (exclusive) */
		if (i < len) i++;                            /* step past '\n' */

		const char *p = buf + ls;
		int n = le - ls;
		int k = skip_ws(p, n);
		p += k; n -= k;
		if (n == 0 || p[0] == '#') continue;         /* blank / comment */

		/* key = up to ':' */
		int ki = 0;
		while (ki < n && p[ki] != ':') ki++;
		if (ki >= n) continue;                       /* no colon -> not a setting */
		int klen = ki;
		while (klen > 0 && (p[klen - 1] == ' ' || p[klen - 1] == '\t')) klen--;

		/* value = after ':' (trim leading + trailing ws) */
		const char *v = p + ki + 1;
		int vn = n - ki - 1;
		int vk = skip_ws(v, vn); v += vk; vn -= vk;
		while (vn > 0 && (v[vn - 1] == ' ' || v[vn - 1] == '\t' || v[vn - 1] == '\r')) vn--;

		if      (tok_eq(p, klen, "blur"))               s->blur = parse_bool(v, vn, s->blur);
		else if (tok_eq(p, klen, "blur_level"))         s->blur_level = clamp100(parse_int(v, vn));
		else if (tok_eq(p, klen, "transparency"))       s->transparency = parse_bool(v, vn, s->transparency);
		else if (tok_eq(p, klen, "transparency_level")) s->transparency_level = clamp100(parse_int(v, vn));
	}
}

/* append NUL-terminated `str` to out[*pos, cap); returns 0 on overflow. */
static int emit(char *out, int *pos, int cap, const char *str)
{
	int i = 0;
	while (str[i]) {
		if (*pos >= cap - 1) return 0;
		out[(*pos)++] = str[i++];
	}
	return 1;
}

/* append a 0..100 integer */
static int emit_int(char *out, int *pos, int cap, int v)
{
	char tmp[4]; int t = 0;
	if (v <= 0) { tmp[t++] = '0'; }
	else { char rev[4]; int r = 0; while (v > 0 && r < 4) { rev[r++] = (char) ('0' + v % 10); v /= 10; }
	       while (r > 0) tmp[t++] = rev[--r]; }
	tmp[t] = 0;
	return emit(out, pos, cap, tmp);
}

int nw_settings_serialize(const struct nw_settings *s, char *out, int cap)
{
	int pos = 0;
	int ok = 1;
	ok &= emit(out, &pos, cap, "blur: ");
	ok &= emit(out, &pos, cap, s->blur ? "true\n" : "false\n");
	ok &= emit(out, &pos, cap, "blur_level: ");
	ok &= emit_int(out, &pos, cap, clamp100(s->blur_level));
	ok &= emit(out, &pos, cap, "\n");
	ok &= emit(out, &pos, cap, "transparency: ");
	ok &= emit(out, &pos, cap, s->transparency ? "true\n" : "false\n");
	ok &= emit(out, &pos, cap, "transparency_level: ");
	ok &= emit_int(out, &pos, cap, clamp100(s->transparency_level));
	ok &= emit(out, &pos, cap, "\n");
	if (!ok) return 0;
	out[pos] = 0;
	return pos;
}

static int clamp_alpha(int a) { return a < 130 ? 130 : (a > 255 ? 255 : a); }

int nw_settings_win_alpha(const struct nw_settings *s)
{
	if (!s->transparency) return 255;
	return clamp_alpha(255 - clamp100(s->transparency_level));
}

int nw_settings_dark_alpha(const struct nw_settings *s)
{
	if (!s->transparency) return 255;
	return clamp_alpha(255 - clamp100(s->transparency_level) + 8);
}

int nw_settings_blur_radius(const struct nw_settings *s)
{
	if (!s->blur) return 0;
	int r = clamp100(s->blur_level) * 40 / 100;
	return r < 1 ? 1 : r;
}
