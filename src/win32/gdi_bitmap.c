/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gdi32: bitmaps (device-dependent ones and DIB sections), converting to
 * and from DIBs, and blits.
 *
 * Every pixel operation goes through a "surface": a view of rows of
 * pixels in any DIB format (1, 4, 8, 16, 24 or 32 bits, colour tables and
 * bit-field masks) that reads and writes COLORREFs. Copies between
 * matching formats take a memcpy fast path; everything else converts pixel
 * by pixel, mapping colours to the nearest palette entry where the target
 * is indexed. Stretching is nearest-neighbour whatever the stretch mode,
 * and raster operations work on RGB values rather than palette indices.
 *
 * A device-dependent bitmap is stored top-down with WORD-aligned rows (the
 * GetBitmapBits layout); a DIB section as its BITMAPINFO describes. A DIB
 * section's bits are page-aligned, as on Windows.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "win32/gdi.h"
#include "win32/internal.h"

#define MAX_BITMAP_BYTES (512u << 20)

extern const struct builtin_dll builtin_kernel32;

struct surface {
	BYTE *bits;
	int width, height, stride, bpp, top_down;
	DWORD masks[3];
	int shift[3], nbits[3];
	const RGBQUAD *colors;
	int ncolors;
	COLORREF last;		/* the nearest-colour cache */
	int last_index;
};

/* A parsed BITMAPINFO. */
struct dib {
	int width, height, top_down, bpp, stride;
	DWORD compression;
	DWORD masks[3];
	RGBQUAD colors[256];
	int ncolors;
	BITMAPINFOHEADER *hdr;
};

static RGBQUAD quad(BYTE r, BYTE g, BYTE b)
{
	RGBQUAD q = { b, g, r, 0 };
	return q;
}

/* The colour table GDI gives a bpp format that has none of its own. */
static int default_colors(int bpp, RGBQUAD *out)
{
	static const BYTE vga[16][3] = {
		{ 0, 0, 0 }, { 0x80, 0, 0 }, { 0, 0x80, 0 }, { 0x80, 0x80, 0 },
		{ 0, 0, 0x80 }, { 0x80, 0, 0x80 }, { 0, 0x80, 0x80 },
		{ 0x80, 0x80, 0x80 }, { 0xc0, 0xc0, 0xc0 }, { 0xff, 0, 0 },
		{ 0, 0xff, 0 }, { 0xff, 0xff, 0 }, { 0, 0, 0xff },
		{ 0xff, 0, 0xff }, { 0, 0xff, 0xff }, { 0xff, 0xff, 0xff },
	};
	PALETTEENTRY half[256];

	switch (bpp) {
	case 1:
		out[0] = quad(0, 0, 0);
		out[1] = quad(255, 255, 255);
		return 2;
	case 4:
		for (int i = 0; i < 16; i++)
			out[i] = quad(vga[i][0], vga[i][1], vga[i][2]);
		return 16;
	case 8:
		gdi_halftone_palette(half);
		for (int i = 0; i < 256; i++)
			out[i] = quad(half[i].peRed, half[i].peGreen, half[i].peBlue);
		return 256;
	default:
		return 0;
	}
}

static void default_masks(int bpp, DWORD masks[3])
{
	if (bpp == 16) {
		masks[0] = 0x7c00;
		masks[1] = 0x03e0;
		masks[2] = 0x001f;
	} else {
		masks[0] = 0xff0000;
		masks[1] = 0x00ff00;
		masks[2] = 0x0000ff;
	}
}

/* Surfaces. */

static void surf_init(struct surface *s, BYTE *bits, int width, int height,
		      int stride, int bpp, int top_down, const DWORD *masks,
		      const RGBQUAD *colors, int ncolors)
{
	memset(s, 0, sizeof(*s));
	s->bits = bits;
	s->width = width;
	s->height = height;
	s->stride = stride;
	s->bpp = bpp;
	s->top_down = top_down;
	s->colors = colors;
	s->ncolors = ncolors;
	s->last = CLR_INVALID;
	if (bpp == 16 || bpp == 32) {
		for (int c = 0; c < 3; c++) {
			DWORD m = s->masks[c] = masks[c];
			while (m && !(m & 1)) {
				m >>= 1;
				s->shift[c]++;
			}
			while (m & 1) {
				m >>= 1;
				s->nbits[c]++;
			}
		}
	}
}

static void surf_bitmap(struct surface *s, struct gdi_bitmap *b)
{
	surf_init(s, b->bits, b->width, b->height, b->stride, b->bpp,
		  b->top_down, b->masks, b->colors, b->ncolors);
}

static BYTE *surf_row(const struct surface *s, int y)
{
	return s->bits + (size_t)(s->top_down ? y : s->height - 1 - y) * s->stride;
}

static BYTE get_ch(const struct surface *s, DWORD v, int c)
{
	int n = s->nbits[c];
	uint64_t max = n ? ((uint64_t)1 << n) - 1 : 0;
	if (!max)
		return 0;
	return (BYTE)((((v & s->masks[c]) >> s->shift[c]) * 255 + max / 2) / max);
}

static DWORD put_ch(const struct surface *s, BYTE v, int c)
{
	int n = s->nbits[c];
	uint64_t max = n ? ((uint64_t)1 << n) - 1 : 0;
	return (DWORD)(((v * max + 127) / 255) << s->shift[c]) & s->masks[c];
}

static COLORREF px_get(const struct surface *s, int x, int y)
{
	const BYTE *row = surf_row(s, y);
	unsigned i;
	DWORD v;

	switch (s->bpp) {
	case 1:
		i = (row[x >> 3] >> (7 - (x & 7))) & 1;
		break;
	case 4:
		i = (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15;
		break;
	case 8:
		i = row[x];
		break;
	case 16:
		v = row[2 * x] | row[2 * x + 1] << 8;
		return RGB(get_ch(s, v, 0), get_ch(s, v, 1), get_ch(s, v, 2));
	case 24:
		return RGB(row[3 * x + 2], row[3 * x + 1], row[3 * x]);
	default:
		memcpy(&v, row + 4 * x, 4);
		return RGB(get_ch(s, v, 0), get_ch(s, v, 1), get_ch(s, v, 2));
	}
	if ((int)i >= s->ncolors)
		return 0;
	return RGB(s->colors[i].rgbRed, s->colors[i].rgbGreen, s->colors[i].rgbBlue);
}

static int nearest(struct surface *s, COLORREF c)
{
	long best_d = -1;
	int best = 0;

	if (c == s->last)
		return s->last_index;
	for (int i = 0; i < s->ncolors; i++) {
		long dr = s->colors[i].rgbRed - GetRValue(c);
		long dg = s->colors[i].rgbGreen - GetGValue(c);
		long db = s->colors[i].rgbBlue - GetBValue(c);
		long d = dr * dr + dg * dg + db * db;
		if (best_d < 0 || d < best_d) {
			best_d = d;
			best = i;
			if (!d)
				break;
		}
	}
	s->last = c;
	s->last_index = best;
	return best;
}

static void px_put(struct surface *s, int x, int y, COLORREF c)
{
	BYTE *row = surf_row(s, y);
	DWORD v;
	int i;

	switch (s->bpp) {
	case 1:
		i = nearest(s, c);
		row[x >> 3] = (row[x >> 3] & ~(0x80 >> (x & 7))) | (i << (7 - (x & 7)));
		break;
	case 4:
		i = nearest(s, c);
		if (x & 1)
			row[x >> 1] = (row[x >> 1] & 0xf0) | i;
		else
			row[x >> 1] = (row[x >> 1] & 0x0f) | i << 4;
		break;
	case 8:
		row[x] = nearest(s, c);
		break;
	case 16:
		v = put_ch(s, GetRValue(c), 0) | put_ch(s, GetGValue(c), 1) |
		    put_ch(s, GetBValue(c), 2);
		row[2 * x] = (BYTE)v;
		row[2 * x + 1] = (BYTE)(v >> 8);
		break;
	case 24:
		row[3 * x] = GetBValue(c);
		row[3 * x + 1] = GetGValue(c);
		row[3 * x + 2] = GetRValue(c);
		break;
	default:
		v = put_ch(s, GetRValue(c), 0) | put_ch(s, GetGValue(c), 1) |
		    put_ch(s, GetBValue(c), 2);
		memcpy(row + 4 * x, &v, 4);
		break;
	}
}

/* Whether pixels can be copied between a and b byte for byte. */
static int same_format(const struct surface *a, const struct surface *b)
{
	if (a->bpp != b->bpp)
		return 0;
	if (a->bpp == 24)
		return 1;
	if (a->bpp > 8)
		return !memcmp(a->masks, b->masks, sizeof(a->masks));
	return a->ncolors == b->ncolors &&
	       !memcmp(a->colors, b->colors, a->ncolors * sizeof(RGBQUAD));
}

/* Copies width pixels of row sy of src to row dy of dst. */
static void copy_row(struct surface *dst, int dy, const struct surface *src,
		     int sy, int width)
{
	if (same_format(dst, src) && (width * dst->bpp) % 8 == 0) {
		memcpy(surf_row(dst, dy), surf_row(src, sy), width * dst->bpp / 8);
		return;
	}
	for (int x = 0; x < width; x++)
		px_put(dst, x, dy, px_get(src, x, sy));
}

/* Raster operations. */

static int rop_uses_src(DWORD rop)
{
	switch (rop) {
	case SRCCOPY:
	case NOTSRCCOPY:
	case SRCPAINT:
	case SRCAND:
	case SRCINVERT:
	case SRCERASE:
	case NOTSRCERASE:
		return 1;
	default:
		return 0;
	}
}

static int rop_supported(DWORD rop)
{
	return rop_uses_src(rop) || rop == PATCOPY || rop == PATINVERT ||
	       rop == DSTINVERT || rop == BLACKNESS || rop == WHITENESS;
}

static COLORREF rop_apply(DWORD rop, COLORREF s, COLORREF d, COLORREF p)
{
	switch (rop) {
	case SRCCOPY:     return s;
	case NOTSRCCOPY:  return ~s & 0xffffff;
	case SRCPAINT:    return s | d;
	case SRCAND:      return s & d;
	case SRCINVERT:   return s ^ d;
	case SRCERASE:    return s & ~d & 0xffffff;
	case NOTSRCERASE: return ~(s | d) & 0xffffff;
	case PATCOPY:     return p;
	case PATINVERT:   return p ^ d;
	case DSTINVERT:   return ~d & 0xffffff;
	case BLACKNESS:   return 0;
	default:          return 0xffffff;	/* WHITENESS */
	}
}

/*
 * Blits the rectangle (sx, sy, sw, sh) of s (NULL: a black source) onto
 * (dx, dy, dw, dh) of d, stretching and mirroring as the signs and sizes
 * say, clipped to both surfaces. Coordinates are top-down.
 */
static void blit(struct surface *d, int dx, int dy, int dw, int dh,
		 const struct surface *s, int sx, int sy, int sw, int sh,
		 DWORD rop, COLORREF pat)
{
	int fx = 0, fy = 0;

	/* A negative extent spans x + w + 1 .. x, as GDI's mirroring does. */
	if (dw < 0) { dx += dw + 1; dw = -dw; fx ^= 1; }
	if (dh < 0) { dy += dh + 1; dh = -dh; fy ^= 1; }
	if (sw < 0) { sx += sw + 1; sw = -sw; fx ^= 1; }
	if (sh < 0) { sy += sh + 1; sh = -sh; fy ^= 1; }
	if (!dw || !dh || (s && (!sw || !sh)))
		return;

	if (s && rop == SRCCOPY && dw == sw && dh == sh && !fx && !fy &&
	    d->bpp >= 8 && same_format(d, s)) {
		int x0 = dx < 0 ? -dx : 0, y0 = dy < 0 ? -dy : 0;
		if (sx + x0 < 0)
			x0 = -sx;
		if (sy + y0 < 0)
			y0 = -sy;
		int w = dw, h = dh;
		if (dx + w > d->width) w = d->width - dx;
		if (sx + w > s->width) w = s->width - sx;
		if (dy + h > d->height) h = d->height - dy;
		if (sy + h > s->height) h = s->height - sy;
		int bytes = d->bpp / 8;
		for (int j = y0; j < h && w > x0; j++)
			memcpy(surf_row(d, dy + j) + (dx + x0) * bytes,
			       surf_row(s, sy + j) + (sx + x0) * bytes,
			       (w - x0) * bytes);
		return;
	}

	int uses_dst = rop != SRCCOPY && rop != NOTSRCCOPY && rop != PATCOPY &&
		       rop != BLACKNESS && rop != WHITENESS;
	for (int j = 0; j < dh; j++) {
		int y = dy + j;
		if (y < 0 || y >= d->height)
			continue;
		int sj = fy ? dh - 1 - j : j;
		int syp = s ? sy + (int)((int64_t)sj * sh / dh) : 0;
		if (s && (syp < 0 || syp >= s->height))
			continue;
		for (int i = 0; i < dw; i++) {
			int x = dx + i;
			if (x < 0 || x >= d->width)
				continue;
			COLORREF sc = 0, dc = 0;
			if (s) {
				int si = fx ? dw - 1 - i : i;
				int sxp = sx + (int)((int64_t)si * sw / dw);
				if (sxp < 0 || sxp >= s->width)
					continue;
				sc = px_get(s, sxp, syp);
			}
			if (uses_dst)
				dc = px_get(d, x, y);
			px_put(d, x, y, rop_apply(rop, sc, dc, pat));
		}
	}
}

/* Bitmap objects. */

static void *kernel32_fn(const char *name)
{
	return builtin_export(&builtin_kernel32, name);
}

void gdi_bitmap_free(struct gdi_bitmap *b)
{
	if (b->owns_bits && b->bits)
		munmap(b->bits, (size_t)b->stride * b->height);
	if (b->view) {
		BOOL (WINAPI *unmap)(LPCVOID) = kernel32_fn("UnmapViewOfFile");
		if (unmap)
			unmap(b->view);
	}
	b->bits = NULL;
	b->view = NULL;
}

static int alloc_bits(struct gdi_bitmap *b)
{
	size_t size = (size_t)b->stride * b->height;
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED)
		return -1;
	b->bits = p;
	b->owns_bits = 1;
	return 0;
}

/* Sets up b's geometry and format; bits are left to the caller. */
static int bitmap_setup(struct gdi_bitmap *b, int width, int height, int bpp,
			int dib, int top_down)
{
	memset(b, 0, sizeof(*b));
	if (width <= 0 || height <= 0) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return -1;
	}
	b->width = width;
	b->height = height;
	b->bpp = bpp;
	b->dib = dib;
	b->top_down = top_down;
	b->stride = dib ? ((width * bpp + 31) / 32) * 4 :
			  ((width * bpp + 15) / 16) * 2;
	if ((uint64_t)width * bpp / 8 > MAX_BITMAP_BYTES ||
	    (uint64_t)b->stride * height > MAX_BITMAP_BYTES) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return -1;
	}
	b->compression = BI_RGB;
	default_masks(bpp, b->masks);
	b->ncolors = default_colors(bpp, b->colors);
	return 0;
}

void gdi_bitmap_init_default(struct gdi_bitmap *b)
{
	if (!bitmap_setup(b, 1, 1, 1, 0, 1))
		alloc_bits(b);
}

/*
 * A new bitmap object, with zeroed bits if alloc is set. Called with the
 * lock held; NULL (with the last error set) on failure.
 */
static struct gdi_obj *new_bitmap(int width, int height, int bpp, int dib,
				  int top_down, int alloc)
{
	struct gdi_obj *o = gdi_new(OBJ_BITMAP);

	if (!o)
		return NULL;
	if (bitmap_setup(&o->u.bmp, width, height, bpp, dib, top_down)) {
		gdi_free(o);
		return NULL;
	}
	if (alloc && alloc_bits(&o->u.bmp)) {
		gdi_free(o);
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	return o;
}

/* The bit counts GDI stores: others are rounded up to one of these. */
static int round_bpp(int bpp)
{
	if (bpp <= 1)
		return 1;
	if (bpp <= 4)
		return 4;
	if (bpp <= 8)
		return 8;
	if (bpp <= 16)
		return 16;
	if (bpp <= 24)
		return 24;
	return 32;
}

/*
 * Parses a BITMAPINFO (a BITMAPINFOHEADER or a V4/V5 header; not the old
 * BITMAPCOREHEADER). DIB_PAL_COLORS tables are resolved through dc's
 * palette. Returns 0, or -1 for a format GDI doesn't take.
 */
static int parse_dib(const BITMAPINFO *bmi, UINT usage, const struct gdi_dc *dc,
		     struct dib *d)
{
	const BITMAPINFOHEADER *h = bmi ? &bmi->bmiHeader : NULL;

	memset(d, 0, sizeof(*d));
	if (!h || h->biSize < sizeof(BITMAPINFOHEADER) || h->biWidth <= 0 ||
	    !h->biHeight || h->biPlanes != 1)
		return -1;
	d->bpp = h->biBitCount;
	d->compression = h->biCompression;
	if (d->bpp != 1 && d->bpp != 4 && d->bpp != 8 && d->bpp != 16 &&
	    d->bpp != 24 && d->bpp != 32)
		return -1;
	if (d->compression != BI_RGB &&
	    !(d->compression == BI_BITFIELDS && (d->bpp == 16 || d->bpp == 32)))
		return -1;
	d->width = h->biWidth;
	d->top_down = h->biHeight < 0;
	d->height = d->top_down ? -h->biHeight : h->biHeight;
	d->stride = ((d->width * d->bpp + 31) / 32) * 4;
	if ((uint64_t)d->stride * d->height > MAX_BITMAP_BYTES)
		return -1;
	d->hdr = (BITMAPINFOHEADER *)h;

	const BYTE *table = (const BYTE *)h + h->biSize;
	default_masks(d->bpp, d->masks);
	if (d->compression == BI_BITFIELDS) {
		/* V4 and V5 headers hold the masks; plain ones precede the table. */
		const BYTE *m = h->biSize >= 52 ? (const BYTE *)h + 40 : table;
		memcpy(d->masks, m, sizeof(d->masks));
		if (h->biSize == sizeof(BITMAPINFOHEADER))
			table += sizeof(d->masks);
	}
	if (d->bpp <= 8) {
		int max = 1 << d->bpp;
		d->ncolors = h->biClrUsed && (int)h->biClrUsed < max ?
			     (int)h->biClrUsed : max;
		for (int i = 0; i < d->ncolors; i++) {
			if (usage == DIB_PAL_COLORS) {
				COLORREF c = gdi_palette_color(dc, ((const WORD *)table)[i]);
				d->colors[i] = quad(GetRValue(c), GetGValue(c), GetBValue(c));
			} else {
				d->colors[i] = ((const RGBQUAD *)table)[i];
			}
		}
	}
	return 0;
}

/* A surface over rows scanlines of a DIB, stored as the DIB stores them. */
static void surf_dib(struct surface *s, const struct dib *d, const void *bits,
		     int rows)
{
	surf_init(s, (BYTE *)bits, d->width, rows, d->stride, d->bpp,
		  d->top_down, d->masks, d->colors, d->ncolors);
}

/* The surface a DC draws on, or 0 for the display (which has none). */
static int dc_surface(struct gdi_obj *dco, struct surface *s)
{
	struct gdi_obj *b;

	if (!dco->u.dc.memory || !(b = gdi_get(dco->u.dc.s.bitmap, OBJ_BITMAP)))
		return 0;
	surf_bitmap(s, &b->u.bmp);
	return 1;
}

/* The DC's brush colour; 0 if the brush paints nothing. */
static int dc_pattern(struct gdi_obj *dco, COLORREF *pat)
{
	struct gdi_obj *br = gdi_get(dco->u.dc.s.brush, OBJ_BRUSH);

	if (!br || br->u.brush.lbStyle == BS_NULL)
		return 0;
	*pat = br->u.brush.lbColor & 0xffffff;
	return 1;
}

/* Creating bitmaps. */

WINFN(HBITMAP) gdi_CreateBitmap(int width, int height, UINT planes, UINT bpp,
				LPCVOID bits)
{
	HBITMAP h = NULL;

	if (!width || !height)
		return gdi_default_bitmap();
	gdi_lock();
	struct gdi_obj *o = new_bitmap(width < 0 ? -width : width,
				       height < 0 ? -height : height,
				       round_bpp(planes * bpp), 0, 1, 1);
	if (o) {
		if (bits)
			memcpy(o->u.bmp.bits, bits,
			       (size_t)o->u.bmp.stride * o->u.bmp.height);
		h = o->handle;
	}
	gdi_unlock();
	return h;
}

WINFN(HBITMAP) gdi_CreateBitmapIndirect(const BITMAP *bm)
{
	if (!bm) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	return gdi_CreateBitmap(bm->bmWidth, bm->bmHeight, bm->bmPlanes,
				bm->bmBitsPixel, bm->bmBits);
}

/*
 * A device-dependent bitmap in hdc's format: the display's, or that of the
 * bitmap selected into a memory DC (monochrome for a fresh one, as on
 * Windows). With no DC at all, monochrome. Called with the lock held.
 */
static struct gdi_obj *compatible_bitmap(HDC hdc, int width, int height)
{
	struct gdi_obj *d = hdc ? gdi_get(hdc, OBJ_DC) : NULL, *b = NULL;
	int bpp = hdc ? GDI_SCREEN_BPP : 1;

	if (hdc && !d)
		return NULL;
	if (d && d->u.dc.memory && (b = gdi_get(d->u.dc.s.bitmap, OBJ_BITMAP)))
		bpp = b->u.bmp.bpp;
	struct gdi_obj *o = new_bitmap(width, height, bpp, 0, 1, 1);
	if (o && b) {
		o->u.bmp.compression = b->u.bmp.compression;
		memcpy(o->u.bmp.masks, b->u.bmp.masks, sizeof(o->u.bmp.masks));
		memcpy(o->u.bmp.colors, b->u.bmp.colors, sizeof(o->u.bmp.colors));
		o->u.bmp.ncolors = b->u.bmp.ncolors;
	}
	return o;
}

WINFN(HBITMAP) gdi_CreateCompatibleBitmap(HDC hdc, int width, int height)
{
	if (!width || !height)
		return gdi_default_bitmap();
	gdi_lock();
	struct gdi_obj *o = compatible_bitmap(hdc, width < 0 ? -width : width,
					      height < 0 ? -height : height);
	gdi_unlock();
	return o ? o->handle : NULL;
}

WINFN(HBITMAP) gdi_CreateDIBSection(HDC hdc, const BITMAPINFO *bmi, UINT usage,
				    void **bits, HANDLE section, DWORD offset)
{
	struct dib d;
	HBITMAP h = NULL;

	if (bits)
		*bits = NULL;
	gdi_lock();
	struct gdi_obj *dc = hdc ? gdi_get(hdc, OBJ_DC) : NULL;
	if ((hdc && !dc) || parse_dib(bmi, usage, dc ? &dc->u.dc : NULL, &d)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		goto out;
	}
	struct gdi_obj *o = new_bitmap(d.width, d.height, d.bpp, 1, d.top_down,
				       !section);
	if (!o)
		goto out;
	struct gdi_bitmap *b = &o->u.bmp;
	if (section) {
		LPVOID (WINAPI *map)(HANDLE, DWORD, DWORD, DWORD, SIZE_T) =
			kernel32_fn("MapViewOfFile");
		DWORD base = offset & ~0xffffu;
		BYTE *view = map ? map(section, FILE_MAP_READ | FILE_MAP_WRITE, 0,
				       base, offset - base +
				       (SIZE_T)b->stride * b->height) : NULL;
		if (!view) {
			gdi_free(o);
			goto out;
		}
		b->view = view;
		b->bits = view + (offset - base);
		b->section = section;
		b->offset = offset;
	}
	b->compression = d.compression;
	memcpy(b->masks, d.masks, sizeof(b->masks));
	if (d.bpp <= 8) {
		memcpy(b->colors, d.colors, sizeof(b->colors));
		b->ncolors = d.ncolors;
	}
	if (bits)
		*bits = b->bits;
	h = o->handle;
	TRACE("CreateDIBSection(%dx%d, %d bpp) = %p, bits %p", d.width,
	      d.top_down ? -d.height : d.height, d.bpp, (void *)h, (void *)b->bits);
out:
	gdi_unlock();
	return h;
}

/*
 * Copies lines scanlines of the DIB d, starting at scanline start, from
 * bits into b (to_dib == 0) or from b into bits. Scanlines count from the
 * bottom of a bottom-up DIB and the top of a top-down one, and meet the
 * bitmap's rows from the same edge. Returns the number copied.
 */
static int transfer(struct gdi_bitmap *b, const struct dib *d, UINT start,
		    UINT lines, void *bits, int to_dib)
{
	struct surface bs, ds;

	if (start >= (UINT)d->height)
		return 0;
	if (lines > d->height - start)
		lines = d->height - start;
	surf_bitmap(&bs, b);
	surf_dib(&ds, d, bits, lines);
	int width = d->width < b->width ? d->width : b->width;
	for (UINT r = 0; r < lines; r++) {
		/* Surface rows are top-down within the band. */
		UINT scan = d->top_down ? start + r : start + (lines - 1 - r);
		int y = d->top_down ? (int)scan : b->height - 1 - (int)scan;
		if (y < 0 || y >= b->height)
			continue;
		if (to_dib)
			copy_row(&ds, r, &bs, y, width);
		else
			copy_row(&bs, y, &ds, r, width);
	}
	return lines;
}

WINFN(int) gdi_SetDIBits(HDC hdc, HBITMAP hbm, UINT start, UINT lines,
			 LPCVOID bits, const BITMAPINFO *bmi, UINT usage)
{
	struct dib d;
	int r = 0;

	gdi_lock();
	struct gdi_obj *dc = hdc ? gdi_get(hdc, OBJ_DC) : NULL;
	struct gdi_obj *o = gdi_get(hbm, OBJ_BITMAP);
	if (o && bits && !parse_dib(bmi, usage, dc ? &dc->u.dc : NULL, &d))
		r = transfer(&o->u.bmp, &d, start, lines, (void *)bits, 0);
	else if (o)
		win_set_error(ERROR_INVALID_PARAMETER);
	gdi_unlock();
	return r;
}

/* Fills in the colour table (or masks) GetDIBits returns for format d. */
static void out_table(BITMAPINFO *bmi, struct dib *d, const struct gdi_bitmap *b,
		      UINT usage)
{
	BITMAPINFOHEADER *h = &bmi->bmiHeader;
	BYTE *table = (BYTE *)h + h->biSize;

	if (d->compression == BI_BITFIELDS) {
		/* The bitmap's own masks, the caller's, or the defaults. */
		if (b->bpp == d->bpp)
			memcpy(d->masks, b->masks, sizeof(d->masks));
		else if (!d->masks[0] && !d->masks[1] && !d->masks[2])
			default_masks(d->bpp, d->masks);
		if (h->biSize == sizeof(BITMAPINFOHEADER))
			memcpy(table, d->masks, sizeof(d->masks));
	}
	if (d->bpp > 8)
		return;
	if (b->bpp == d->bpp && b->ncolors) {
		memcpy(d->colors, b->colors, sizeof(d->colors));
		d->ncolors = b->ncolors;
	} else {
		d->ncolors = default_colors(d->bpp, d->colors);
	}
	h->biClrUsed = 0;
	for (int i = 0; i < (1 << d->bpp); i++) {
		if (usage == DIB_PAL_COLORS)
			((WORD *)table)[i] = i;
		else
			((RGBQUAD *)table)[i] = i < d->ncolors ? d->colors[i] :
								 quad(0, 0, 0);
	}
}

WINFN(int) gdi_GetDIBits(HDC hdc, HBITMAP hbm, UINT start, UINT lines,
			 LPVOID bits, LPBITMAPINFO bmi, UINT usage)
{
	struct dib d;
	int r = 0;

	gdi_lock();
	struct gdi_obj *dc = gdi_get(hdc, OBJ_DC), *o = gdi_get(hbm, OBJ_BITMAP);
	BITMAPINFOHEADER *h = bmi ? &bmi->bmiHeader : NULL;
	if (!dc || !o || !h || h->biSize < sizeof(BITMAPINFOHEADER)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		goto out;
	}
	struct gdi_bitmap *b = &o->u.bmp;
	if (!h->biBitCount) {
		/* Just describe the bitmap. */
		h->biWidth = b->width;
		h->biHeight = b->dib && b->top_down ? -b->height : b->height;
		h->biPlanes = 1;
		h->biBitCount = b->bpp;
		h->biCompression = b->compression;
		h->biSizeImage = ((b->width * b->bpp + 31) / 32) * 4 * b->height;
		h->biXPelsPerMeter = h->biYPelsPerMeter = 0;
		h->biClrUsed = h->biClrImportant = 0;
		r = b->height;
		goto out;
	}
	if (parse_dib(bmi, DIB_RGB_COLORS, NULL, &d)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		goto out;
	}
	out_table(bmi, &d, b, usage);
	if (usage == DIB_PAL_COLORS)
		for (int i = 0; i < d.ncolors; i++) {
			COLORREF c = gdi_palette_color(&dc->u.dc, i);
			d.colors[i] = quad(GetRValue(c), GetGValue(c), GetBValue(c));
		}
	if (d.compression == BI_RGB || d.compression == BI_BITFIELDS)
		h->biSizeImage = d.stride * d.height;
	if (!bits) {
		r = d.height;
		goto out;
	}
	r = transfer(b, &d, start, lines, bits, 1);
out:
	gdi_unlock();
	return r;
}

WINFN(LONG) gdi_GetBitmapBits(HBITMAP hbm, LONG count, LPVOID bits)
{
	LONG r = 0;

	gdi_lock();
	struct gdi_obj *o = gdi_get(hbm, OBJ_BITMAP);
	if (o) {
		const struct gdi_bitmap *b = &o->u.bmp;
		struct surface s;
		int row = ((b->width * b->bpp + 15) / 16) * 2;
		surf_bitmap(&s, (struct gdi_bitmap *)b);
		if (!bits) {
			r = row * b->height;
		} else {
			for (int y = 0; y < b->height && count >= row; y++) {
				memcpy((BYTE *)bits + r, surf_row(&s, y), row);
				r += row;
				count -= row;
			}
		}
	}
	gdi_unlock();
	return r;
}

WINFN(LONG) gdi_SetBitmapBits(HBITMAP hbm, DWORD count, LPCVOID bits)
{
	LONG r = 0;

	gdi_lock();
	struct gdi_obj *o = gdi_get(hbm, OBJ_BITMAP);
	if (o && bits && !o->stock) {
		struct gdi_bitmap *b = &o->u.bmp;
		struct surface s;
		DWORD row = ((b->width * b->bpp + 15) / 16) * 2;
		surf_bitmap(&s, b);
		for (int y = 0; y < b->height && count >= row; y++) {
			memcpy(surf_row(&s, y), (const BYTE *)bits + r, row);
			r += row;
			count -= row;
		}
	}
	gdi_unlock();
	return r;
}

WINFN(HBITMAP) gdi_CreateDIBitmap(HDC hdc, const BITMAPINFOHEADER *hdr,
				  DWORD init, LPCVOID bits,
				  const BITMAPINFO *bmi, UINT usage)
{
	HBITMAP h = NULL;

	if (!hdr || hdr->biWidth <= 0 || !hdr->biHeight) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	gdi_lock();
	struct gdi_obj *o = compatible_bitmap(hdc, hdr->biWidth,
					      hdr->biHeight < 0 ? -hdr->biHeight :
								  hdr->biHeight);
	if (o) {
		h = o->handle;
		if ((init & CBM_INIT) && bits && bmi) {
			struct gdi_obj *dc = hdc ? gdi_get(hdc, OBJ_DC) : NULL;
			struct dib d;
			if (!parse_dib(bmi, usage, dc ? &dc->u.dc : NULL, &d))
				transfer(&o->u.bmp, &d, 0, d.height, (void *)bits, 0);
		}
	}
	gdi_unlock();
	return h;
}

/* Blits. */

WINFN(BOOL) gdi_StretchBlt(HDC hdc, int x, int y, int w, int h, HDC src,
			   int sx, int sy, int sw, int sh, DWORD rop)
{
	BOOL ok = FALSE;

	if (!rop_supported(rop)) {
		TRACE("StretchBlt: raster operation %#x not supported", rop);
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	gdi_lock();
	struct gdi_obj *d = gdi_get(hdc, OBJ_DC);
	struct gdi_obj *s = rop_uses_src(rop) ? gdi_get(src, OBJ_DC) : NULL;
	struct surface ds, ss;
	COLORREF pat = 0;
	if (!d || (rop_uses_src(rop) && !s))
		goto out;
	ok = TRUE;
	int has_pat = dc_pattern(d, &pat);
	if ((rop == PATCOPY || rop == PATINVERT) && !has_pat)
		goto out;
	if (!dc_surface(d, &ds))
		goto out;		/* the display: nothing to draw on */
	int has_src = s && dc_surface(s, &ss);
	BYTE *copy = NULL;
	if (has_src && ss.bits == ds.bits) {
		size_t size = (size_t)ss.stride * ss.height;
		if (!(copy = malloc(size))) {
			ok = FALSE;
			goto out;
		}
		memcpy(copy, ss.bits, size);
		ss.bits = copy;
	}
	blit(&ds, x, y, w, h, has_src ? &ss : NULL, sx, sy, sw, sh, rop, pat);
	free(copy);
out:
	gdi_unlock();
	return ok;
}

WINFN(BOOL) gdi_BitBlt(HDC hdc, int x, int y, int w, int h, HDC src, int sx,
		       int sy, DWORD rop)
{
	return gdi_StretchBlt(hdc, x, y, w, h, src, sx, sy, w, h, rop);
}

WINFN(BOOL) gdi_PatBlt(HDC hdc, int x, int y, int w, int h, DWORD rop)
{
	if (rop_uses_src(rop)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	return gdi_StretchBlt(hdc, x, y, w, h, NULL, 0, 0, w, h, rop);
}

int gdi_fill_rect(HDC hdc, const RECT *rc, HBRUSH brush)
{
	int ok = 0;

	gdi_lock();
	struct gdi_obj *d = gdi_get(hdc, OBJ_DC), *br = gdi_get(brush, OBJ_BRUSH);
	struct surface ds;
	if (d && br && rc) {
		ok = 1;
		if (br->u.brush.lbStyle != BS_NULL && dc_surface(d, &ds))
			blit(&ds, rc->left, rc->top, rc->right - rc->left,
			     rc->bottom - rc->top, NULL, 0, 0, 0, 0, PATCOPY,
			     br->u.brush.lbColor & 0xffffff);
	}
	gdi_unlock();
	return ok;
}

/* A COLORREF as RGB: PALETTEINDEX ones from the DC's palette. */
static COLORREF dc_rgb(const struct gdi_obj *d, COLORREF c)
{
	if ((c >> 24) == 1)
		return gdi_palette_color(&d->u.dc, (WORD)c);
	return c & 0xffffff;
}

/* rc cut to clip (if any) and to the surface; 0 if nothing is left. */
static int clip_rect(RECT *rc, const RECT *clip, const struct surface *s)
{
	if (clip) {
		if (rc->left < clip->left) rc->left = clip->left;
		if (rc->top < clip->top) rc->top = clip->top;
		if (rc->right > clip->right) rc->right = clip->right;
		if (rc->bottom > clip->bottom) rc->bottom = clip->bottom;
	}
	if (rc->left < 0) rc->left = 0;
	if (rc->top < 0) rc->top = 0;
	if (rc->right > s->width) rc->right = s->width;
	if (rc->bottom > s->height) rc->bottom = s->height;
	return rc->left < rc->right && rc->top < rc->bottom;
}

int gdi_fill_color(HDC hdc, const RECT *rc, COLORREF c, const RECT *clip)
{
	struct surface s;
	int ok = 0;

	gdi_lock();
	struct gdi_obj *d = gdi_get(hdc, OBJ_DC);
	if (d && rc) {
		ok = 1;
		RECT r = *rc;
		if (dc_surface(d, &s) && clip_rect(&r, clip, &s)) {
			COLORREF rgb = dc_rgb(d, c);
			for (int y = r.top; y < r.bottom; y++)
				for (int x = r.left; x < r.right; x++)
					px_put(&s, x, y, rgb);
		}
	}
	gdi_unlock();
	return ok;
}

int gdi_paint_mask(HDC hdc, int x, int y, int w, int h, const BYTE *mask,
		   COLORREF fg, const COLORREF *bg, const RECT *clip)
{
	struct surface s;
	int ok = 0;

	gdi_lock();
	struct gdi_obj *d = gdi_get(hdc, OBJ_DC);
	if (d && mask && w >= 0 && h >= 0) {
		ok = 1;
		RECT r = { x, y, x + w, y + h };
		if (dc_surface(d, &s) && clip_rect(&r, clip, &s)) {
			COLORREF ink = dc_rgb(d, fg), paper = bg ? dc_rgb(d, *bg) : 0;
			for (int py = r.top; py < r.bottom; py++)
				for (int px = r.left; px < r.right; px++) {
					if (mask[(size_t)(py - y) * w + (px - x)])
						px_put(&s, px, py, ink);
					else if (bg)
						px_put(&s, px, py, paper);
				}
		}
	}
	gdi_unlock();
	return ok;
}

/*
 * A DIB onto a DC. ySrc counts from the bottom of a bottom-up DIB, as the
 * DIB's own scanlines do.
 */
WINFN(int) gdi_StretchDIBits(HDC hdc, int x, int y, int w, int h, int sx,
			     int sy, int sw, int sh, LPCVOID bits,
			     const BITMAPINFO *bmi, UINT usage, DWORD rop)
{
	struct dib dib;
	struct surface ds, ss;
	int r = 0;

	if (!rop_supported(rop) || !bits) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return 0;
	}
	gdi_lock();
	struct gdi_obj *d = gdi_get(hdc, OBJ_DC);
	COLORREF pat = 0;
	if (!d || parse_dib(bmi, usage, &d->u.dc, &dib)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		goto out;
	}
	r = sh < 0 ? -sh : sh;
	dc_pattern(d, &pat);
	if (dc_surface(d, &ds)) {
		surf_dib(&ss, &dib, bits, dib.height);
		int top = dib.top_down ? sy : dib.height - sy - sh;
		blit(&ds, x, y, w, h, &ss, sx, top, sw, sh, rop, pat);
	}
out:
	gdi_unlock();
	return r;
}

/*
 * lines scanlines of a DIB, from scanline start, onto a DC without
 * stretching; (sx, sy) is in the whole DIB's coordinates, from its bottom
 * for a bottom-up one.
 */
WINFN(int) gdi_SetDIBitsToDevice(HDC hdc, int x, int y, DWORD w, DWORD h,
				 int sx, int sy, UINT start, UINT lines,
				 LPCVOID bits, const BITMAPINFO *bmi,
				 UINT usage)
{
	struct dib dib;
	struct surface ds, ss;
	int r = 0;

	if (!bits) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return 0;
	}
	gdi_lock();
	struct gdi_obj *d = gdi_get(hdc, OBJ_DC);
	if (!d || parse_dib(bmi, usage, &d->u.dc, &dib)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		goto out;
	}
	r = lines;
	if (dc_surface(d, &ds) && lines) {
		/* A surface over just the band that was passed. */
		surf_dib(&ss, &dib, bits, lines);
		int top = dib.top_down ? sy - (int)start :
					 (int)(start + lines) - sy - (int)h;
		blit(&ds, x, y, w, h, &ss, sx, top, w, h, SRCCOPY, 0);
	}
out:
	gdi_unlock();
	return r;
}

/* Pixels and colour tables. */

WINFN(COLORREF) gdi_GetPixel(HDC hdc, int x, int y)
{
	COLORREF c = CLR_INVALID;
	struct surface s;

	gdi_lock();
	struct gdi_obj *d = gdi_get(hdc, OBJ_DC);
	if (d && dc_surface(d, &s)) {
		if (x >= 0 && y >= 0 && x < s.width && y < s.height)
			c = px_get(&s, x, y);
	} else if (d && x >= 0 && y >= 0 && x < GDI_SCREEN_WIDTH &&
		   y < GDI_SCREEN_HEIGHT) {
		c = 0;
	}
	gdi_unlock();
	return c;
}

WINFN(COLORREF) gdi_SetPixel(HDC hdc, int x, int y, COLORREF c)
{
	COLORREF r = CLR_INVALID;
	struct surface s;

	gdi_lock();
	struct gdi_obj *d = gdi_get(hdc, OBJ_DC);
	if (d && dc_surface(d, &s)) {
		if (x >= 0 && y >= 0 && x < s.width && y < s.height) {
			px_put(&s, x, y, c & 0xffffff);
			r = px_get(&s, x, y);
		}
	} else if (d) {
		r = c & 0xffffff;
	}
	gdi_unlock();
	return r;
}

WINFN(BOOL) gdi_SetPixelV(HDC hdc, int x, int y, COLORREF c)
{
	return gdi_SetPixel(hdc, x, y, c) != CLR_INVALID;
}

static UINT color_table(HDC hdc, UINT start, UINT n, RGBQUAD *colors, int set)
{
	UINT r = 0;

	gdi_lock();
	struct gdi_obj *d = gdi_get(hdc, OBJ_DC), *o = NULL;
	if (d && d->u.dc.memory)
		o = gdi_get(d->u.dc.s.bitmap, OBJ_BITMAP);
	if (o && o->u.bmp.dib && o->u.bmp.bpp <= 8 && colors) {
		struct gdi_bitmap *b = &o->u.bmp;
		int max = 1 << b->bpp;
		if (start < (UINT)max) {
			r = max - start < n ? max - start : n;
			if (set) {
				memcpy(&b->colors[start], colors, r * sizeof(*colors));
				if ((int)(start + r) > b->ncolors)
					b->ncolors = start + r;
			} else {
				memcpy(colors, &b->colors[start], r * sizeof(*colors));
			}
		}
	}
	gdi_unlock();
	return r;
}

WINFN(UINT) gdi_GetDIBColorTable(HDC hdc, UINT start, UINT n, RGBQUAD *colors)
{
	return color_table(hdc, start, n, colors, 0);
}

WINFN(UINT) gdi_SetDIBColorTable(HDC hdc, UINT start, UINT n,
				 const RGBQUAD *colors)
{
	return color_table(hdc, start, n, (RGBQUAD *)colors, 1);
}

#define E(name) { #name, 0, (void *)gdi_##name }

const struct builtin_export gdi_bitmap_exports[] = {
	E(BitBlt),
	E(CreateBitmap),
	E(CreateBitmapIndirect),
	E(CreateCompatibleBitmap),
	E(CreateDIBSection),
	E(CreateDIBitmap),
	E(GetBitmapBits),
	E(GetDIBColorTable),
	E(GetDIBits),
	E(GetPixel),
	E(PatBlt),
	E(SetBitmapBits),
	E(SetDIBColorTable),
	E(SetDIBits),
	E(SetDIBitsToDevice),
	E(SetPixel),
	E(SetPixelV),
	E(StretchBlt),
	E(StretchDIBits),
	BUILTIN_END
};
