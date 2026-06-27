#include "nw_settings.h"

void nw_settings_defaults(struct nw_settings *s)
{
	s->blur               = 0;    /* off: the backdrop blur is the expensive path */
	s->blur_level         = 60;
	s->transparency       = 1;    /* glass on */
	s->transparency_level = 50;
	s->accent             = 0x12a8f4u;   /* NanoOS blue */
	s->wallpaper          = NW_WALL_BRANDED;
	s->wallpaper_color    = 0x1e2a3au;   /* slate, for solid mode */
	s->clock_24h          = 1;
	s->clock_seconds      = 0;
	s->shadow             = 1;
	s->corner_radius      = 11;
	{ const char *d = NW_UI_FONT_DEFAULT; int i = 0; for (; d[i] && i < 63; i++) s->ui_font[i] = d[i]; s->ui_font[i] = 0; }
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

/* Parse a colour: hex with optional 0x/# prefix (0x12a8f4, #12a8f4, 12a8f4). */
static unsigned parse_hex(const char *tok, int tn)
{
	int i = 0;
	if (i < tn && tok[i] == '#') i++;
	else if (i + 1 < tn && tok[i] == '0' && (tok[i + 1] == 'x' || tok[i + 1] == 'X')) i += 2;
	unsigned v = 0;
	for (; i < tn; i++) {
		char c = tok[i]; int d;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
		else break;
		v = (v << 4) | (unsigned) d;
	}
	return v & 0xffffffu;
}

static int parse_wallpaper(const char *tok, int tn, int fallback)
{
	if (tok_eq(tok, tn, "branded"))  return NW_WALL_BRANDED;
	if (tok_eq(tok, tn, "gradient")) return NW_WALL_GRADIENT;
	if (tok_eq(tok, tn, "solid"))    return NW_WALL_SOLID;
	return fallback;
}

static int clamp_radius(int v) { return v < 0 ? 0 : (v > 20 ? 20 : v); }

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
		else if (tok_eq(p, klen, "accent"))             s->accent = parse_hex(v, vn);
		else if (tok_eq(p, klen, "wallpaper"))          s->wallpaper = parse_wallpaper(v, vn, s->wallpaper);
		else if (tok_eq(p, klen, "wallpaper_color"))    s->wallpaper_color = parse_hex(v, vn);
		else if (tok_eq(p, klen, "clock_24h"))          s->clock_24h = parse_bool(v, vn, s->clock_24h);
		else if (tok_eq(p, klen, "clock_seconds"))      s->clock_seconds = parse_bool(v, vn, s->clock_seconds);
		else if (tok_eq(p, klen, "shadow"))             s->shadow = parse_bool(v, vn, s->shadow);
		else if (tok_eq(p, klen, "corner_radius"))      s->corner_radius = clamp_radius(parse_int(v, vn));
		else if (tok_eq(p, klen, "ui_font")) {
			int m = vn < 63 ? vn : 63; if (m < 0) m = 0;
			for (int i = 0; i < m; i++) s->ui_font[i] = v[i];
			s->ui_font[m] = 0;
		}
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

/* append "0xRRGGBB" */
static int emit_hex(char *out, int *pos, int cap, unsigned v)
{
	static const char H[] = "0123456789abcdef";
	if (!emit(out, pos, cap, "0x")) return 0;
	for (int sh = 20; sh >= 0; sh -= 4) {
		if (*pos >= cap - 1) return 0;
		out[(*pos)++] = H[(v >> sh) & 0xf];
	}
	return 1;
}

static const char *wall_name(int w)
{
	return w == NW_WALL_GRADIENT ? "gradient" : (w == NW_WALL_SOLID ? "solid" : "branded");
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
	ok &= emit(out, &pos, cap, "accent: ");
	ok &= emit_hex(out, &pos, cap, s->accent);
	ok &= emit(out, &pos, cap, "\n");
	ok &= emit(out, &pos, cap, "wallpaper: ");
	ok &= emit(out, &pos, cap, wall_name(s->wallpaper));
	ok &= emit(out, &pos, cap, "\n");
	ok &= emit(out, &pos, cap, "wallpaper_color: ");
	ok &= emit_hex(out, &pos, cap, s->wallpaper_color);
	ok &= emit(out, &pos, cap, "\n");
	ok &= emit(out, &pos, cap, "clock_24h: ");
	ok &= emit(out, &pos, cap, s->clock_24h ? "true\n" : "false\n");
	ok &= emit(out, &pos, cap, "clock_seconds: ");
	ok &= emit(out, &pos, cap, s->clock_seconds ? "true\n" : "false\n");
	ok &= emit(out, &pos, cap, "shadow: ");
	ok &= emit(out, &pos, cap, s->shadow ? "true\n" : "false\n");
	ok &= emit(out, &pos, cap, "corner_radius: ");
	ok &= emit_int(out, &pos, cap, clamp_radius(s->corner_radius));
	ok &= emit(out, &pos, cap, "\n");
	ok &= emit(out, &pos, cap, "ui_font: ");
	ok &= emit(out, &pos, cap, s->ui_font[0] ? s->ui_font : NW_UI_FONT_DEFAULT);
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
