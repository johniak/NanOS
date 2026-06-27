/*
 * png.c — minimal PNG decoder (see png.h). Self-contained: a compact DEFLATE inflater (puff-style,
 * after Mark Adler's public-domain reference) + PNG chunk parsing + scanline defiltering. Handles
 * non-interlaced 8-bit greyscale / RGB / RGBA / palette images — enough for the wallpaper.
 */
#include "nwui_png.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* ---- DEFLATE (RFC 1951) inflate ------------------------------------------------------------ */

struct instate {
	const uint8_t *in;  unsigned inlen, incnt;
	int bitbuf, bitcnt;
	uint8_t *out; unsigned outlen, outcnt;   /* grown by the caller's estimate; we bounds-check */
};

static int bits(struct instate *s, int need)
{
	long val = s->bitbuf;
	while (s->bitcnt < need) {
		if (s->incnt == s->inlen) return -1;
		val |= (long) (s->in[s->incnt++]) << s->bitcnt;
		s->bitcnt += 8;
	}
	s->bitbuf = (int) (val >> need);
	s->bitcnt -= need;
	return (int) (val & ((1L << need) - 1));
}

struct huff { short *count; short *symbol; };

static int decode(struct instate *s, const struct huff *h)
{
	int code = 0, first = 0, index = 0, len, count, bit;
	for (len = 1; len <= 15; len++) {
		if (s->incnt == s->inlen && s->bitcnt == 0) return -1;
		bit = bits(s, 1);
		if (bit < 0) return -1;
		code |= bit;
		count = h->count[len];
		if (code - count < first) return h->symbol[index + (code - first)];
		index += count; first += count; first <<= 1; code <<= 1;
	}
	return -1;
}

static int construct(struct huff *h, const short *length, int n)
{
	int symbol, len, left;
	short offs[16];
	for (len = 0; len <= 15; len++) h->count[len] = 0;
	for (symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
	if (h->count[0] == n) return 0;
	left = 1;
	for (len = 1; len <= 15; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return left; }
	offs[1] = 0;
	for (len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->count[len];
	for (symbol = 0; symbol < n; symbol++)
		if (length[symbol] != 0) h->symbol[offs[length[symbol]]++] = symbol;
	return left;
}

static int put(struct instate *s, int byte)
{
	if (s->outcnt >= s->outlen) return -1;
	s->out[s->outcnt++] = (uint8_t) byte;
	return 0;
}

static int codes(struct instate *s, const struct huff *lencode, const struct huff *distcode)
{
	static const short lens[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
	static const short lext[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
	static const short dists[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
	static const short dext[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
	int symbol, len, dist, i;
	do {
		symbol = decode(s, lencode);
		if (symbol < 0) return symbol;
		if (symbol < 256) { if (put(s, symbol)) return -1; }
		else if (symbol > 256) {
			symbol -= 257;
			if (symbol >= 29) return -1;
			len = lens[symbol] + bits(s, lext[symbol]);
			symbol = decode(s, distcode);
			if (symbol < 0) return symbol;
			dist = dists[symbol] + bits(s, dext[symbol]);
			if ((unsigned) dist > s->outcnt) return -1;
			for (i = 0; i < len; i++) { if (put(s, s->out[s->outcnt - dist])) return -1; }
		}
	} while (symbol != 256);
	return 0;
}

static int fixed_block(struct instate *s)
{
	short lcount[16], lsym[288], dcount[16], dsym[30], lengths[288];
	struct huff lc = { lcount, lsym }, dc = { dcount, dsym };
	int i;
	for (i = 0; i < 144; i++) lengths[i] = 8;
	for (; i < 256; i++) lengths[i] = 9;
	for (; i < 280; i++) lengths[i] = 7;
	for (; i < 288; i++) lengths[i] = 8;
	construct(&lc, lengths, 288);
	for (i = 0; i < 30; i++) lengths[i] = 5;
	construct(&dc, lengths, 30);
	return codes(s, &lc, &dc);
}

static int dynamic_block(struct instate *s)
{
	static const short order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
	short lcount[16], lsym[288], dcount[16], dsym[30];
	short lengths[288 + 32];
	struct huff lc = { lcount, lsym }, dc = { dcount, dsym };
	int nlen, ndist, ncode, index, err, symbol, len;
	nlen = bits(s, 5) + 257; ndist = bits(s, 5) + 1; ncode = bits(s, 4) + 4;
	if (nlen > 286 || ndist > 30) return -1;
	for (index = 0; index < ncode; index++) lengths[order[index]] = (short) bits(s, 3);
	for (; index < 19; index++) lengths[order[index]] = 0;
	err = construct(&lc, lengths, 19);
	if (err) return -1;
	index = 0;
	while (index < nlen + ndist) {
		symbol = decode(s, &lc);
		if (symbol < 0) return -1;
		if (symbol < 16) lengths[index++] = (short) symbol;
		else {
			len = 0;
			if (symbol == 16) { if (index == 0) return -1; len = lengths[index - 1]; symbol = 3 + bits(s, 2); }
			else if (symbol == 17) symbol = 3 + bits(s, 3);
			else symbol = 11 + bits(s, 7);
			if (index + symbol > nlen + ndist) return -1;
			while (symbol--) lengths[index++] = (short) len;
		}
	}
	if (lengths[256] == 0) return -1;
	err = construct(&lc, lengths, nlen);
	if (err && (err < 0 || nlen != lc.count[0] + lc.count[1])) return -1;
	err = construct(&dc, lengths + nlen, ndist);
	if (err && (err < 0 || ndist != dc.count[0] + dc.count[1])) return -1;
	return codes(s, &lc, &dc);
}

static int stored_block(struct instate *s)
{
	unsigned len;
	s->bitbuf = 0; s->bitcnt = 0;
	if (s->incnt + 4 > s->inlen) return -1;
	len = s->in[s->incnt++]; len |= s->in[s->incnt++] << 8;
	s->incnt += 2;   /* skip the one's-complement length */
	if (s->incnt + len > s->inlen) return -1;
	while (len--) { if (put(s, s->in[s->incnt++])) return -1; }
	return 0;
}

/* Inflate a raw DEFLATE stream into out[outlen]. Returns bytes produced, or -1. */
static long inflate_raw(const uint8_t *in, unsigned inlen, uint8_t *out, unsigned outlen)
{
	struct instate s;
	int last, type, err;
	s.in = in; s.inlen = inlen; s.incnt = 0; s.bitbuf = 0; s.bitcnt = 0;
	s.out = out; s.outlen = outlen; s.outcnt = 0;
	do {
		last = bits(&s, 1);
		type = bits(&s, 2);
		if (type == 0) err = stored_block(&s);
		else if (type == 1) err = fixed_block(&s);
		else if (type == 2) err = dynamic_block(&s);
		else return -1;
		if (err) return -1;
	} while (!last);
	return (long) s.outcnt;
}

/* ---- PNG ----------------------------------------------------------------------------------- */

static unsigned be32(const uint8_t *p) { return ((unsigned) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static int paeth(int a, int b, int c)
{
	int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
	if (pa <= pb && pa <= pc) return a;
	return pb <= pc ? b : c;
}

uint32_t *png_decode(const uint8_t *data, unsigned len, int *wout, int *hout)
{
	static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	if (len < 8 || memcmp(data, sig, 8) != 0) return 0;

	unsigned w = 0, h = 0;
	int bitdepth = 0, colortype = 0, interlace = 0;
	uint8_t pal[256 * 3]; int paln = 0;
	/* Concatenate IDAT payloads into one buffer (zlib stream). */
	uint8_t *idat = 0; unsigned idatlen = 0;

	unsigned off = 8;
	while (off + 8 <= len) {
		unsigned clen = be32(data + off);
		const uint8_t *ctype = data + off + 4;
		const uint8_t *cdata = data + off + 8;
		if (off + 12 + clen > len) break;
		if (memcmp(ctype, "IHDR", 4) == 0 && clen >= 13) {
			w = be32(cdata); h = be32(cdata + 4);
			bitdepth = cdata[8]; colortype = cdata[9]; interlace = cdata[12];
		} else if (memcmp(ctype, "PLTE", 4) == 0) {
			paln = (int) (clen / 3); if (paln > 256) paln = 256;
			memcpy(pal, cdata, (size_t) paln * 3);
		} else if (memcmp(ctype, "IDAT", 4) == 0) {
			uint8_t *n = (uint8_t *) realloc(idat, idatlen + clen);
			if (!n) { free(idat); return 0; }
			idat = n; memcpy(idat + idatlen, cdata, clen); idatlen += clen;
		} else if (memcmp(ctype, "IEND", 4) == 0) {
			break;
		}
		off += 12 + clen;   /* length + type + data + CRC */
	}

	if (!idat || w == 0 || h == 0 || bitdepth != 8 || interlace != 0) { free(idat); return 0; }
	int channels;
	switch (colortype) {
	case 0: channels = 1; break;   /* greyscale */
	case 2: channels = 3; break;   /* RGB */
	case 3: channels = 1; break;   /* palette index */
	case 4: channels = 2; break;   /* grey + alpha */
	case 6: channels = 4; break;   /* RGBA */
	default: free(idat); return 0;
	}
	if (colortype == 3 && paln == 0) { free(idat); return 0; }

	/* Decompress: zlib stream = 2-byte header + DEFLATE (we ignore the trailing Adler-32). */
	unsigned stride = w * (unsigned) channels;
	unsigned rawlen = (stride + 1) * h;          /* one filter byte per scanline */
	uint8_t *raw = (uint8_t *) malloc(rawlen);
	if (!raw) { free(idat); return 0; }
	long got = (idatlen > 2) ? inflate_raw(idat + 2, idatlen - 2, raw, rawlen) : -1;
	free(idat);
	if (got != (long) rawlen) { free(raw); return 0; }

	/* Defilter in place into contiguous pixel rows (drop the per-row filter byte). */
	uint8_t *img = (uint8_t *) malloc((size_t) stride * h);
	if (!img) { free(raw); return 0; }
	int bpp = channels;                          /* bytes per pixel (bitdepth 8) */
	for (unsigned y = 0; y < h; y++) {
		uint8_t f = raw[y * (stride + 1)];
		const uint8_t *src = raw + y * (stride + 1) + 1;
		uint8_t *row = img + (size_t) y * stride;
		uint8_t *prev = y ? img + (size_t) (y - 1) * stride : 0;
		for (unsigned x = 0; x < stride; x++) {
			int a = (int) x >= bpp ? row[x - bpp] : 0;
			int b = prev ? prev[x] : 0;
			int c = (prev && (int) x >= bpp) ? prev[x - bpp] : 0;
			int v = src[x];
			switch (f) {
			case 0: break;
			case 1: v += a; break;
			case 2: v += b; break;
			case 3: v += (a + b) / 2; break;
			case 4: v += paeth(a, b, c); break;
			default: free(raw); free(img); return 0;
			}
			row[x] = (uint8_t) v;
		}
	}
	free(raw);

	/* Expand to 0x00RRGGBB. */
	uint32_t *px = (uint32_t *) malloc((size_t) w * h * 4);
	if (!px) { free(img); return 0; }
	for (unsigned y = 0; y < h; y++) {
		for (unsigned x = 0; x < w; x++) {
			const uint8_t *p = img + (size_t) y * stride + (size_t) x * bpp;
			uint8_t r, g, bl, a = 255;
			if (colortype == 2 || colortype == 6) { r = p[0]; g = p[1]; bl = p[2]; if (colortype == 6) a = p[3]; }
			else if (colortype == 3) { const uint8_t *e = pal + p[0] * 3; r = e[0]; g = e[1]; bl = e[2]; }
			else { r = g = bl = p[0]; if (colortype == 4) a = p[1]; }   /* greyscale (+ alpha) */
			/* 0xAARRGGBB — alpha PRESERVED (top byte). Opaque-only consumers mask it off. */
			px[(size_t) y * w + x] = ((uint32_t) a << 24) | ((uint32_t) r << 16) | ((uint32_t) g << 8) | bl;
		}
	}
	free(img);
	*wout = (int) w; *hout = (int) h;
	return px;
}

/* Read an entire file into a malloc'd buffer, then png_decode it. Icons/wallpapers are small;
 * cap the read at 256 KiB like nwm's wallpaper loader. */
uint32_t *nwui_image_load_png(const char *path, int *w, int *h)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return 0;
	unsigned cap = 256u * 1024u;
	uint8_t *file = (uint8_t *) malloc(cap);
	if (!file) { close(fd); return 0; }
	int total = 0, got;
	while (total < (int) cap && (got = read(fd, file + total, cap - total)) > 0)
		total += got;
	close(fd);
	uint32_t *px = (total > 0) ? png_decode(file, (unsigned) total, w, h) : 0;
	free(file);
	return px;
}
