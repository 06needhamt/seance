/*
 * DIB frame conversion to BGR24 and I420.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/fourcc.h"
#include "out/image.h"

#define FCC(a, b, c, d) mmioFOURCC(a, b, c, d)
#define MAX_DIM 16384

enum kind {
	K_PAL,		/* 1, 4, 8 bits with a palette */
	K_RLE,		/* RLE8 / RLE4 with a palette */
	K_RGB24,
	K_RGB32,	/* BGRX */
	K_MASKS,	/* 16 or 32 bits with channel masks */
	K_PACKED,	/* 4:2:2 in 4-byte groups */
	K_PLANAR,	/* I420, YV12 */
	K_NV,		/* NV12, NV21 */
	K_GREY,
};

struct image_conv {
	enum kind kind;
	int w, h, bpp;
	int bottom_up;
	size_t stride, frame_size;
	char name[48];

	BYTE pal[256][3];	/* B, G, R */
	int shift[3], bits[3];	/* R, G, B masks */

	int y0, u, y1, v;	/* packed: byte offsets in a group */
	int swap_uv;		/* YV12: V before U; NV21: VU pairs */

	BYTE *rle;		/* RLE: palette indices, bottom row first */
	BYTE *scratch;		/* intermediate frame */
};

static int clamp8(int v)
{
	return v < 0 ? 0 : v > 255 ? 255 : v;
}

static int is_rgb(const struct image_conv *c)
{
	return c->kind <= K_MASKS;
}

size_t image_bgr24_size(int w, int h)
{
	return (size_t)w * 3 * h;
}

size_t image_i420_size(int w, int h)
{
	size_t cw = (w + 1) / 2, ch = (h + 1) / 2;
	return (size_t)w * h + 2 * cw * ch;
}

/* A FOURCC with its letters upper-cased, for matching YUV formats. */
static FOURCC upper_fourcc(FOURCC f)
{
	FOURCC r = 0;
	for (int i = 0; i < 4; i++) {
		unsigned ch = (f >> (8 * i)) & 0xff;
		if (ch >= 'a' && ch <= 'z')
			ch -= 32;
		r |= (FOURCC)ch << (8 * i);
	}
	return r;
}

static int set_mask(struct image_conv *c, int i, DWORD mask)
{
	int shift = 0, bits = 0;

	if (!mask)
		return -1;
	while (!(mask & 1)) {
		mask >>= 1;
		shift++;
	}
	while (mask & 1) {
		mask >>= 1;
		bits++;
	}
	if (mask || bits > 16)
		return -1;	/* not contiguous */
	c->shift[i] = shift;
	c->bits[i] = bits;
	return 0;
}

static int load_palette(struct image_conv *c, const BITMAPINFOHEADER *bih,
			size_t fmt_size)
{
	size_t n = bih->biClrUsed ? bih->biClrUsed : 1u << c->bpp;
	const BYTE *q = (const BYTE *)bih + bih->biSize;

	if (n > 256)
		n = 256;
	if (fmt_size < bih->biSize)
		n = 0;
	else if (n > (fmt_size - bih->biSize) / 4)
		n = (fmt_size - bih->biSize) / 4;
	if (!n) {
		/* No palette: a grey ramp shows something sensible. */
		unsigned max = (1u << c->bpp) - 1;
		for (unsigned i = 0; i <= max; i++)
			memset(c->pal[i], i * 255 / max, 3);
		return 0;
	}
	for (size_t i = 0; i < n; i++)
		memcpy(c->pal[i], q + 4 * i, 3);	/* RGBQUAD is B, G, R, x */
	return 0;
}

static int setup_rgb(struct image_conv *c, const BITMAPINFOHEADER *bih,
		     size_t fmt_size, DWORD compression)
{
	c->bpp = bih->biBitCount;
	c->bottom_up = bih->biHeight > 0;

	if (compression == BI_RLE8 || compression == BI_RLE4) {
		if (c->bpp != (compression == BI_RLE8 ? 8 : 4) || !c->bottom_up)
			return -1;
		c->kind = K_RLE;
		snprintf(c->name, sizeof(c->name), "RLE%d", c->bpp);
		c->rle = calloc((size_t)c->w, c->h);
		if (!c->rle)
			return -1;
		return load_palette(c, bih, fmt_size);
	}

	c->stride = ((size_t)c->w * c->bpp + 31) / 32 * 4;
	c->frame_size = c->stride * c->h;
	const char *dir = c->bottom_up ? "bottom-up" : "top-down";

	if (compression == BI_BITFIELDS) {
		const BYTE *m;
		DWORD mask[3];

		if (c->bpp != 16 && c->bpp != 32)
			return -1;
		/* The masks end a V2+ header, or follow a plain one. */
		if (bih->biSize >= 52)
			m = (const BYTE *)bih + 40;
		else if (fmt_size >= (size_t)bih->biSize + 12)
			m = (const BYTE *)bih + bih->biSize;
		else
			return -1;
		memcpy(mask, m, 12);
		for (int i = 0; i < 3; i++)
			if (set_mask(c, i, mask[i]))
				return -1;
		c->kind = K_MASKS;
		snprintf(c->name, sizeof(c->name), "RGB%d%d%d %s",
			 c->bits[0], c->bits[1], c->bits[2], dir);
		return 0;
	}

	switch (c->bpp) {
	case 1:
	case 4:
	case 8:
		c->kind = K_PAL;
		snprintf(c->name, sizeof(c->name), "PAL%d %s", c->bpp, dir);
		return load_palette(c, bih, fmt_size);
	case 16:
		c->kind = K_MASKS;
		set_mask(c, 0, 0x7c00);
		set_mask(c, 1, 0x03e0);
		set_mask(c, 2, 0x001f);
		snprintf(c->name, sizeof(c->name), "RGB555 %s", dir);
		return 0;
	case 24:
		c->kind = K_RGB24;
		snprintf(c->name, sizeof(c->name), "RGB24 %s", dir);
		return 0;
	case 32:
		c->kind = K_RGB32;
		snprintf(c->name, sizeof(c->name), "RGB32 %s", dir);
		return 0;
	}
	return -1;
}

static int setup_yuv(struct image_conv *c, FOURCC f)
{
	size_t cw = (c->w + 1) / 2, ch = (c->h + 1) / 2;
	const char *name;

	switch (f) {
	case FCC('Y', 'U', 'Y', '2'): case FCC('Y', 'U', 'Y', 'V'):
	case FCC('Y', 'U', 'N', 'V'): case FCC('V', '4', '2', '2'):
		c->kind = K_PACKED;
		c->y0 = 0; c->u = 1; c->y1 = 2; c->v = 3;
		name = "YUY2";
		break;
	case FCC('U', 'Y', 'V', 'Y'): case FCC('Y', '4', '2', '2'):
	case FCC('U', 'Y', 'N', 'V'): case FCC('H', 'D', 'Y', 'C'):
		c->kind = K_PACKED;
		c->u = 0; c->y0 = 1; c->v = 2; c->y1 = 3;
		name = "UYVY";
		break;
	case FCC('Y', 'V', 'Y', 'U'):
		c->kind = K_PACKED;
		c->y0 = 0; c->v = 1; c->y1 = 2; c->u = 3;
		name = "YVYU";
		break;
	case FCC('I', '4', '2', '0'): case FCC('I', 'Y', 'U', 'V'):
		c->kind = K_PLANAR;
		name = "I420";
		break;
	case FCC('Y', 'V', '1', '2'):
		c->kind = K_PLANAR;
		c->swap_uv = 1;
		name = "YV12";
		break;
	case FCC('N', 'V', '1', '2'):
		c->kind = K_NV;
		name = "NV12";
		break;
	case FCC('N', 'V', '2', '1'):
		c->kind = K_NV;
		c->swap_uv = 1;
		name = "NV21";
		break;
	case FCC('Y', '8', '0', '0'): case FCC('Y', '8', ' ', ' '):
	case FCC('G', 'R', 'E', 'Y'):
		c->kind = K_GREY;
		name = "Y800";
		break;
	default:
		return -1;
	}
	snprintf(c->name, sizeof(c->name), "%s", name);

	switch (c->kind) {
	case K_PACKED:
		c->stride = cw * 4;
		c->frame_size = c->stride * c->h;
		break;
	case K_PLANAR:
	case K_NV:
		c->stride = c->w;
		c->frame_size = (size_t)c->w * c->h + 2 * cw * ch;
		break;
	default:
		c->stride = c->w;
		c->frame_size = (size_t)c->w * c->h;
		break;
	}
	return 0;
}

struct image_conv *image_conv_new(const BITMAPINFOHEADER *bih,
				  size_t fmt_size)
{
	struct image_conv *c;
	DWORD comp;
	char fcc[FOURCC_STR_SIZE];

	if (fmt_size < sizeof(*bih) || bih->biSize < sizeof(*bih) ||
	    bih->biSize > fmt_size) {
		vfw_set_error("bad BITMAPINFOHEADER");
		return NULL;
	}
	LONG h = bih->biHeight < 0 ? -bih->biHeight : bih->biHeight;
	if (bih->biWidth <= 0 || bih->biWidth > MAX_DIM || h <= 0 ||
	    h > MAX_DIM) {
		vfw_set_error("unsupported frame size %ldx%ld",
			      (long)bih->biWidth, (long)bih->biHeight);
		return NULL;
	}
	if (!(c = calloc(1, sizeof(*c)))) {
		vfw_set_error("out of memory");
		return NULL;
	}
	c->w = bih->biWidth;
	c->h = h;

	comp = bih->biCompression;
	if (comp == FCC('D', 'I', 'B', ' ') || comp == FCC('R', 'G', 'B', ' ') ||
	    comp == FCC('R', 'A', 'W', ' '))
		comp = BI_RGB;
	int r;
	if (comp == BI_RGB || comp == BI_BITFIELDS || comp == BI_RLE8 ||
	    comp == BI_RLE4)
		r = setup_rgb(c, bih, fmt_size, comp);
	else
		r = setup_yuv(c, upper_fourcc(comp));
	if (r) {
		if (comp <= BI_PNG)
			vfw_set_error("unsupported uncompressed format "
				      "(compression %lu, %u bits)",
				      (unsigned long)comp, bih->biBitCount);
		else
			vfw_set_error("'%s' is compressed (or unknown): it needs "
				      "a codec", fourcc_str(comp, fcc));
		image_conv_free(c);
		return NULL;
	}

	size_t scratch = image_i420_size(c->w, c->h) > image_bgr24_size(c->w, c->h) ?
			 image_i420_size(c->w, c->h) : image_bgr24_size(c->w, c->h);
	if (!(c->scratch = malloc(scratch))) {
		vfw_set_error("out of memory");
		image_conv_free(c);
		return NULL;
	}
	return c;
}

void image_conv_free(struct image_conv *c)
{
	if (!c)
		return;
	free(c->rle);
	free(c->scratch);
	free(c);
}

int image_width(const struct image_conv *c) { return c->w; }
int image_height(const struct image_conv *c) { return c->h; }
size_t image_frame_size(const struct image_conv *c) { return c->frame_size; }
const char *image_format_name(const struct image_conv *c) { return c->name; }

int image_is_delta(const struct image_conv *c)
{
	return c->kind == K_RLE;
}

int image_palette_change(struct image_conv *c, const void *data, size_t size)
{
	const BYTE *p = data;

	if (c->kind != K_PAL && c->kind != K_RLE)
		return 0;	/* palette changes don't apply */
	if (size < 4) {
		vfw_set_error("short palette change");
		return -1;
	}
	unsigned first = p[0], n = p[1] ? p[1] : 256;
	if (first + n > 256 || size < 4 + 4 * n) {
		vfw_set_error("bad palette change");
		return -1;
	}
	for (unsigned i = 0; i < n; i++) {
		const BYTE *e = p + 4 + 4 * i;	/* PALETTEENTRY: R, G, B, flags */
		c->pal[first + i][0] = e[2];
		c->pal[first + i][1] = e[1];
		c->pal[first + i][2] = e[0];
	}
	return 0;
}

/*
 * A channel of `bits` bits to 8, repeating its bits downwards (5 bits
 * abcde become abcdeabc), as GDI and most decoders do.
 */
static BYTE expand(DWORD v, int bits)
{
	int r = 0;

	v &= (1u << bits) - 1;
	if (bits >= 8)
		return (BYTE)(v >> (bits - 8));
	for (int sh = 8 - bits; sh > -bits; sh -= bits)
		r |= sh >= 0 ? v << sh : v >> -sh;
	return (BYTE)r;
}

/* One row of an RGB-family DIB to BGR24. */
static void rgb_row(const struct image_conv *c, const BYTE *s, BYTE *d)
{
	int w = c->w;

	switch (c->kind) {
	case K_PAL: {
		int bpp = c->bpp, per = 8 / bpp, mask = (1 << bpp) - 1;
		for (int x = 0; x < w; x++) {
			int sh = 8 - bpp * (x % per + 1);
			memcpy(d + 3 * x, c->pal[(s[x / per] >> sh) & mask], 3);
		}
		break;
	}
	case K_RGB24:
		memcpy(d, s, (size_t)w * 3);
		break;
	case K_RGB32:
		for (int x = 0; x < w; x++)
			memcpy(d + 3 * x, s + 4 * x, 3);
		break;
	case K_MASKS:
		for (int x = 0; x < w; x++) {
			DWORD px = c->bpp == 16 ? (DWORD)(s[2 * x] | s[2 * x + 1] << 8) :
				   (DWORD)s[4 * x] | (DWORD)s[4 * x + 1] << 8 |
				   (DWORD)s[4 * x + 2] << 16 | (DWORD)s[4 * x + 3] << 24;
			for (int i = 0; i < 3; i++) {
				d[3 * x + 2 - i] = expand(px >> c->shift[i], c->bits[i]);
			}
		}
		break;
	default:
		break;
	}
}

/* Microsoft RLE: updates the index picture. */
static int decode_rle(struct image_conv *c, const BYTE *s, size_t len)
{
	int w = c->w, h = c->h, four = c->bpp == 4;
	int x = 0, y = 0;
	size_t i = 0;

#define PUT(val) do { if (x < w && y < h) c->rle[(size_t)y * w + x] = (val); \
		      x++; } while (0)
	while (i + 1 < len) {
		BYTE n = s[i], b = s[i + 1];
		i += 2;
		if (n) {
			for (int k = 0; k < n; k++)
				PUT(four ? (k & 1 ? b & 15 : b >> 4) : b);
		} else if (b == 0) {
			x = 0;
			y++;
		} else if (b == 1) {
			return 0;
		} else if (b == 2) {
			if (i + 2 > len)
				break;
			x += s[i];
			y += s[i + 1];
			i += 2;
		} else {
			size_t bytes = four ? (b + 1u) / 2 : b;
			if (i + bytes > len)
				break;
			for (int k = 0; k < b; k++)
				PUT(four ? (k & 1 ? s[i + k / 2] & 15 :
					    s[i + k / 2] >> 4) : s[i + k]);
			i += bytes + (bytes & 1);
		}
		if (y >= h)
			return 0;
	}
#undef PUT
	return 0;	/* no end marker: keep what was decoded */
}

static int check_len(const struct image_conv *c, size_t len)
{
	if (len < c->frame_size) {
		vfw_set_error("frame is %zu bytes, a %dx%d %s frame is %zu",
			      len, c->w, c->h, c->name, c->frame_size);
		return -1;
	}
	return 0;
}

static void rgb_to_bgr24(struct image_conv *c, const BYTE *s, BYTE *d)
{
	size_t out_stride = (size_t)c->w * 3;

	if (c->kind == K_RLE) {
		for (int y = 0; y < c->h; y++) {
			const BYTE *row = c->rle + (size_t)(c->h - 1 - y) * c->w;
			BYTE *o = d + y * out_stride;
			for (int x = 0; x < c->w; x++)
				memcpy(o + 3 * x, c->pal[row[x]], 3);
		}
		return;
	}
	for (int y = 0; y < c->h; y++) {
		int sy = c->bottom_up ? c->h - 1 - y : y;
		rgb_row(c, s + sy * c->stride, d + y * out_stride);
	}
}

/* A YUV-family frame to I420. */
static void yuv_to_i420(const struct image_conv *c, const BYTE *s, BYTE *d)
{
	int w = c->w, h = c->h, cw = (w + 1) / 2, ch = (h + 1) / 2;
	BYTE *dy = d, *du = d + (size_t)w * h, *dv = du + (size_t)cw * ch;

	switch (c->kind) {
	case K_PACKED:
		for (int y = 0; y < h; y++) {
			const BYTE *row = s + y * c->stride;
			for (int x = 0; x < w; x++)
				dy[(size_t)y * w + x] = row[4 * (x / 2) + (x & 1 ? c->y1 : c->y0)];
		}
		for (int y = 0; y < ch; y++) {
			const BYTE *r0 = s + 2 * y * c->stride;
			const BYTE *r1 = 2 * y + 1 < h ? r0 + c->stride : r0;
			for (int x = 0; x < cw; x++) {
				du[(size_t)y * cw + x] = (r0[4 * x + c->u] + r1[4 * x + c->u] + 1) / 2;
				dv[(size_t)y * cw + x] = (r0[4 * x + c->v] + r1[4 * x + c->v] + 1) / 2;
			}
		}
		break;
	case K_PLANAR: {
		const BYTE *su = s + (size_t)w * h, *sv = su + (size_t)cw * ch;
		memcpy(dy, s, (size_t)w * h);
		memcpy(du, c->swap_uv ? sv : su, (size_t)cw * ch);
		memcpy(dv, c->swap_uv ? su : sv, (size_t)cw * ch);
		break;
	}
	case K_NV: {
		const BYTE *uv = s + (size_t)w * h;
		memcpy(dy, s, (size_t)w * h);
		for (size_t i = 0; i < (size_t)cw * ch; i++) {
			du[i] = uv[2 * i + c->swap_uv];
			dv[i] = uv[2 * i + !c->swap_uv];
		}
		break;
	}
	case K_GREY:
		memcpy(dy, s, (size_t)w * h);
		memset(du, 128, 2 * (size_t)cw * ch);
		break;
	default:
		break;
	}
}

int image_feed(struct image_conv *c, const void *src, size_t len)
{
	if (c->kind == K_RLE)
		decode_rle(c, src, len);
	return 0;
}

int image_to_bgr24(struct image_conv *c, const void *src, size_t len,
		   BYTE *dst)
{
	if (c->kind == K_RLE) {
		decode_rle(c, src, len);
		rgb_to_bgr24(c, NULL, dst);
		return 0;
	}
	if (check_len(c, len))
		return -1;
	if (is_rgb(c)) {
		rgb_to_bgr24(c, src, dst);
	} else {
		yuv_to_i420(c, src, c->scratch);
		image_i420_to_bgr24(c->scratch, c->w, c->h, dst);
	}
	return 0;
}

int image_to_i420(struct image_conv *c, const void *src, size_t len,
		  BYTE *dst)
{
	if (c->kind == K_RLE) {
		decode_rle(c, src, len);
		rgb_to_bgr24(c, NULL, c->scratch);
		image_bgr24_to_i420(c->scratch, c->w, c->h, dst);
		return 0;
	}
	if (check_len(c, len))
		return -1;
	if (is_rgb(c)) {
		rgb_to_bgr24(c, src, c->scratch);
		image_bgr24_to_i420(c->scratch, c->w, c->h, dst);
	} else {
		yuv_to_i420(c, src, dst);
	}
	return 0;
}

/* BT.601 limited range. */
static BYTE rgb_y(int r, int g, int b)
{
	return (BYTE)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
}

void image_bgr24_to_i420(const BYTE *bgr, int w, int h, BYTE *d)
{
	int cw = (w + 1) / 2, ch = (h + 1) / 2;
	BYTE *du = d + (size_t)w * h, *dv = du + (size_t)cw * ch;
	size_t stride = (size_t)w * 3;

	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			const BYTE *p = bgr + y * stride + 3 * x;
			d[(size_t)y * w + x] = rgb_y(p[2], p[1], p[0]);
		}
	for (int y = 0; y < ch; y++)
		for (int x = 0; x < cw; x++) {
			int r = 0, g = 0, b = 0, n = 0;
			for (int dy = 0; dy < 2; dy++)
				for (int dx = 0; dx < 2; dx++) {
					int px = 2 * x + dx, py = 2 * y + dy;
					if (px >= w || py >= h)
						continue;
					const BYTE *p = bgr + py * stride + 3 * px;
					b += p[0];
					g += p[1];
					r += p[2];
					n++;
				}
			r = (r + n / 2) / n;
			g = (g + n / 2) / n;
			b = (b + n / 2) / n;
			du[(size_t)y * cw + x] =
				(BYTE)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
			dv[(size_t)y * cw + x] =
				(BYTE)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
		}
}

void image_i420_to_bgr24(const BYTE *yuv, int w, int h, BYTE *d)
{
	int cw = (w + 1) / 2, ch = (h + 1) / 2;
	const BYTE *su = yuv + (size_t)w * h, *sv = su + (size_t)cw * ch;

	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int C = yuv[(size_t)y * w + x] - 16;
			int D = su[(size_t)(y / 2) * cw + x / 2] - 128;
			int E = sv[(size_t)(y / 2) * cw + x / 2] - 128;
			BYTE *p = d + ((size_t)y * w + x) * 3;
			p[0] = clamp8((298 * C + 516 * D + 128) >> 8);
			p[1] = clamp8((298 * C - 100 * D - 208 * E + 128) >> 8);
			p[2] = clamp8((298 * C + 409 * E + 128) >> 8);
		}
}

/* Packing: the inverse of the conversions above. */

int image_can_pack(const struct image_conv *c)
{
	return c->kind != K_PAL && c->kind != K_RLE;
}

int image_pack_wants_i420(const struct image_conv *c)
{
	return !is_rgb(c);
}

/* An 8-bit channel value in `bits` bits, the inverse of expand(). */
static DWORD narrow(int v, int bits)
{
	if (bits >= 8)
		return (DWORD)v << (bits - 8) | (DWORD)v >> (16 - bits);
	DWORD max = (1u << bits) - 1;
	return ((DWORD)v * max + 127) / 255;
}

static void pack_rgb(const struct image_conv *c, const BYTE *bgr, BYTE *dst)
{
	int w = c->w, h = c->h;

	memset(dst, 0, c->frame_size);
	for (int y = 0; y < h; y++) {
		const BYTE *s = bgr + (size_t)(c->bottom_up ? h - 1 - y : y) * w * 3;
		BYTE *d = dst + (size_t)y * c->stride;
		for (int x = 0; x < w; x++, s += 3) {
			switch (c->kind) {
			case K_RGB24:
				memcpy(d + 3 * x, s, 3);
				break;
			case K_RGB32:
				memcpy(d + 4 * x, s, 3);
				break;
			default: {
				DWORD px = 0;
				for (int i = 0; i < 3; i++)
					px |= narrow(s[2 - i], c->bits[i]) << c->shift[i];
				int n = c->bpp / 8;
				for (int k = 0; k < n; k++)
					d[n * x + k] = (BYTE)(px >> (8 * k));
			}
			}
		}
	}
}

static void pack_yuv(const struct image_conv *c, const BYTE *yuv, BYTE *d)
{
	int w = c->w, h = c->h, cw = (w + 1) / 2, ch = (h + 1) / 2;
	const BYTE *sy = yuv, *su = yuv + (size_t)w * h, *sv = su + (size_t)cw * ch;

	switch (c->kind) {
	case K_PACKED:
		for (int y = 0; y < h; y++) {
			BYTE *row = d + (size_t)y * c->stride;
			const BYTE *ly = sy + (size_t)y * w;
			for (int x = 0; x < cw; x++) {
				BYTE *g = row + 4 * x;
				g[c->y0] = ly[2 * x];
				g[c->y1] = 2 * x + 1 < w ? ly[2 * x + 1] : ly[2 * x];
				g[c->u] = su[(size_t)(y / 2) * cw + x];
				g[c->v] = sv[(size_t)(y / 2) * cw + x];
			}
		}
		break;
	case K_PLANAR: {
		BYTE *du = d + (size_t)w * h, *dv = du + (size_t)cw * ch;
		memcpy(d, sy, (size_t)w * h);
		memcpy(c->swap_uv ? dv : du, su, (size_t)cw * ch);
		memcpy(c->swap_uv ? du : dv, sv, (size_t)cw * ch);
		break;
	}
	case K_NV: {
		BYTE *uv = d + (size_t)w * h;
		memcpy(d, sy, (size_t)w * h);
		for (size_t i = 0; i < (size_t)cw * ch; i++) {
			uv[2 * i + c->swap_uv] = su[i];
			uv[2 * i + !c->swap_uv] = sv[i];
		}
		break;
	}
	case K_GREY:
		memcpy(d, sy, (size_t)w * h);
		break;
	default:
		break;
	}
}

int image_pack(const struct image_conv *c, const BYTE *src, BYTE *dst)
{
	if (!image_can_pack(c)) {
		vfw_set_error("can't write frames as %s", c->name);
		return -1;
	}
	if (is_rgb(c))
		pack_rgb(c, src, dst);
	else
		pack_yuv(c, src, dst);
	return 0;
}
