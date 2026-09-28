/*
 * DrawDib: drawing DIBs, compressed or not, onto a DC, after Wine's
 * dlls/msvfw32/drawdib.c. A compressed DIB is decompressed by a codec
 * found for it into a DIB section held in a memory DC, then stretched onto
 * the target DC with StretchBlt; an uncompressed one is copied into the
 * DIB section first. There is no display, so drawing to a window's DC is
 * discarded as gdi32 discards it, but drawing to a memory DC is real,
 * which is how a codec rendering its own frames (ICM_DRAW) can be tested.
 *
 * Unlike Wine, bottom-up and top-down DIBs are both drawn, DDF_DONTDRAW
 * decompresses without drawing, DrawDibUpdate (DDF_UPDATE, no DIB)
 * redraws the last frame, the halftone palette DrawDibRealize makes is
 * freed by DrawDibEnd, and DrawDibChangePalette changes the colours of a
 * palettised buffer.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "vfw/icm.h"
#include "win32/internal.h"

extern const struct builtin_dll builtin_gdi32;

struct hdd {
	HDRAWDIB self;
	struct hdd *next;
	int begun;
	HDC hdc;			/* the DC DrawDibBegin was given */
	int dx_dst, dy_dst, dx_src, dy_src;
	BITMAPINFOHEADER *in;		/* the format begun with */
	size_t in_size;
	BITMAPINFOHEADER *out;		/* the DIB section's format */
	HIC hic;			/* the decompressor, if compressed */
	int started;			/* ICM_DECOMPRESS_BEGIN sent */
	HDC mem;
	HBITMAP dib, old_dib;
	void *bits;
	HPALETTE pal;			/* DrawDibSetPalette / DrawDibRealize */
	HPALETTE own_pal;		/* one DrawDibRealize made */
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct hdd *list;
static ULONG_PTR next_id = 1;

/* gdi32, through its exports. */
static struct {
	HDC (WINAPI *CreateCompatibleDC)(HDC);
	BOOL (WINAPI *DeleteDC)(HDC);
	HBITMAP (WINAPI *CreateDIBSection)(HDC, const BITMAPINFO *, UINT,
					   void **, HANDLE, DWORD);
	HGDIOBJ (WINAPI *SelectObject)(HDC, HGDIOBJ);
	BOOL (WINAPI *DeleteObject)(HGDIOBJ);
	BOOL (WINAPI *StretchBlt)(HDC, int, int, int, int, HDC, int, int, int,
				  int, DWORD);
	HPALETTE (WINAPI *SelectPalette)(HDC, HPALETTE, BOOL);
	UINT (WINAPI *RealizePalette)(HDC);
	HPALETTE (WINAPI *CreateHalftonePalette)(HDC);
	UINT (WINAPI *SetDIBColorTable)(HDC, UINT, UINT, const RGBQUAD *);
} gdi;
static pthread_once_t gdi_once = PTHREAD_ONCE_INIT;

static void gdi_init(void)
{
#define GET(f) (*(void **)&gdi.f = builtin_export(&builtin_gdi32, #f))
	GET(CreateCompatibleDC);
	GET(DeleteDC);
	GET(CreateDIBSection);
	GET(SelectObject);
	GET(DeleteObject);
	GET(StretchBlt);
	GET(SelectPalette);
	GET(RealizePalette);
	GET(CreateHalftonePalette);
	GET(SetDIBColorTable);
#undef GET
}

static struct hdd *get(HDRAWDIB h)
{
	struct hdd *d;

	pthread_mutex_lock(&lock);
	for (d = list; d && d->self != h; d = d->next)
		;
	pthread_mutex_unlock(&lock);
	return d;
}

/* Colour-table entries (or bitfield masks) after a format's header. */
static DWORD num_colours(const BITMAPINFOHEADER *bi)
{
	if (bi->biCompression == BI_BITFIELDS)
		return 3;
	if (bi->biClrUsed)
		return bi->biClrUsed <= 256 ? bi->biClrUsed : 256;
	return bi->biBitCount <= 8 ? 1u << bi->biBitCount : 0;
}

static size_t fmt_size(const BITMAPINFOHEADER *bi)
{
	return bi->biSize + num_colours(bi) * sizeof(RGBQUAD);
}

static LONG abs_height(const BITMAPINFOHEADER *bi)
{
	return bi->biHeight < 0 ? -bi->biHeight : bi->biHeight;
}

static size_t rgb_bytes(const BITMAPINFOHEADER *bi)
{
	return (size_t)((bi->biWidth * bi->biBitCount + 31) / 32 * 4) *
	       abs_height(bi);
}

static int drawable(const BITMAPINFOHEADER *bi)
{
	return bi->biCompression == BI_RGB || bi->biCompression == BI_BITFIELDS;
}

WINFN(HDRAWDIB) vfw_DrawDibOpen(void)
{
	struct hdd *d = calloc(1, sizeof(*d));

	if (!d)
		return NULL;
	pthread_once(&gdi_once, gdi_init);
	pthread_mutex_lock(&lock);
	d->self = (HDRAWDIB)next_id++;
	d->next = list;
	list = d;
	pthread_mutex_unlock(&lock);
	TRACE("DrawDibOpen -> %p", d->self);
	return d->self;
}

static void end(struct hdd *d)
{
	if (d->dib) {
		gdi.SelectObject(d->mem, d->old_dib);
		gdi.DeleteObject(d->dib);
	}
	if (d->mem)
		gdi.DeleteDC(d->mem);
	if (d->hic) {
		if (d->started)
			ic_send(d->hic, ICM_DECOMPRESS_END, 0, 0);
		ic_close(d->hic);
	}
	if (d->own_pal)
		gdi.DeleteObject(d->own_pal);
	free(d->in);
	free(d->out);
	d->in = d->out = NULL;
	d->hic = NULL;
	d->started = 0;
	d->mem = NULL;
	d->dib = d->old_dib = NULL;
	d->bits = NULL;
	d->hdc = NULL;
	d->pal = d->own_pal = NULL;
	d->begun = 0;
}

WINFN(BOOL) vfw_DrawDibEnd(HDRAWDIB h)
{
	struct hdd *d = get(h);

	if (!d)
		return FALSE;
	end(d);
	return TRUE;
}

WINFN(BOOL) vfw_DrawDibClose(HDRAWDIB h)
{
	struct hdd *d = get(h);

	if (!d)
		return FALSE;
	end(d);
	pthread_mutex_lock(&lock);
	for (struct hdd **p = &list; *p; p = &(*p)->next)
		if (*p == d) {
			*p = d->next;
			break;
		}
	pthread_mutex_unlock(&lock);
	free(d);
	return TRUE;
}

/*
 * The format a compressed DIB decompresses to for drawing: the codec's
 * own suggestion if it is RGB, else the display's (32, 24 or 16 bits).
 */
static BITMAPINFOHEADER *draw_format(HIC hic, const BITMAPINFOHEADER *in)
{
	LRESULT err;
	BITMAPINFOHEADER *out = ic_decompress_get_format(hic, in, &err);

	if (out && drawable(out) &&
	    ic_send(hic, ICM_DECOMPRESS_QUERY, (DWORD_PTR)in,
		    (DWORD_PTR)out) == ICERR_OK)
		return out;
	free(out);
	if (!(out = calloc(1, sizeof(struct vfw_format))))
		return NULL;
	if (!ic_get_display_format(hic, (BITMAPINFOHEADER *)in, out, 0, 0, 0)) {
		free(out);
		return NULL;
	}
	return out;
}

static BOOL begin(struct hdd *d, HDC hdc, int dx_dst, int dy_dst,
		  const BITMAPINFOHEADER *bi, int dx_src, int dy_src)
{
	if (d->begun)
		end(d);
	if (!bi || bi->biSize < sizeof(BITMAPINFOHEADER))
		return FALSE;

	if (!drawable(bi)) {
		d->hic = ic_open(ICTYPE_VIDEO, bi->biCompression,
				 ICMODE_DECOMPRESS);
		if (!d->hic)
			d->hic = ic_locate(ICTYPE_VIDEO, 0,
					   (BITMAPINFOHEADER *)bi, NULL,
					   ICMODE_DECOMPRESS);
		if (!d->hic) {
			TRACE("DrawDibBegin: no decompressor for %.4s",
			      (const char *)&bi->biCompression);
			return FALSE;
		}
		if (!(d->out = draw_format(d->hic, bi)) ||
		    ic_send(d->hic, ICM_DECOMPRESS_BEGIN, (DWORD_PTR)bi,
			    (DWORD_PTR)d->out) != ICERR_OK)
			goto fail;
		d->started = 1;
	} else {
		size_t n = fmt_size(bi);
		if (!(d->out = malloc(n)))
			goto fail;
		memcpy(d->out, bi, n);
	}

	d->mem = gdi.CreateCompatibleDC(hdc);
	d->dib = d->mem ? gdi.CreateDIBSection(d->mem, (BITMAPINFO *)d->out,
					       DIB_RGB_COLORS, &d->bits, NULL,
					       0) : NULL;
	if (!d->dib)
		goto fail;
	d->old_dib = gdi.SelectObject(d->mem, d->dib);

	d->in_size = fmt_size(bi);
	if (!(d->in = malloc(d->in_size)))
		goto fail;
	memcpy(d->in, bi, d->in_size);
	d->hdc = hdc;
	d->dx_dst = dx_dst;
	d->dy_dst = dy_dst;
	d->dx_src = dx_src;
	d->dy_src = dy_src;
	d->begun = 1;
	TRACE("DrawDibBegin %ldx%ld %.4s -> %d bpp", (long)bi->biWidth,
	      (long)bi->biHeight, drawable(bi) ? "RGB " :
	      (const char *)&bi->biCompression, d->out->biBitCount);
	return TRUE;

fail:
	end(d);
	return FALSE;
}

WINFN(BOOL) vfw_DrawDibBegin(HDRAWDIB h, HDC hdc, int dx_dst, int dy_dst,
			     LPBITMAPINFOHEADER bi, int dx_src, int dy_src,
			     UINT flags)
{
	struct hdd *d = get(h);

	return d ? begin(d, hdc, dx_dst, dy_dst, bi, dx_src, dy_src) : FALSE;
}

WINFN(BOOL) vfw_DrawDibDraw(HDRAWDIB h, HDC hdc, int x_dst, int y_dst,
			    int dx_dst, int dy_dst, LPBITMAPINFOHEADER bi,
			    LPVOID bits, int x_src, int y_src, int dx_src,
			    int dy_src, UINT flags)
{
	struct hdd *d = get(h);

	if (!d)
		return FALSE;

	if ((flags & DDF_UPDATE) && !bi) {
		/*
		 * DrawDibUpdate: the buffered frame again, at the size it was
		 * begun with, onto whichever DC is given.
		 */
		if (!d->begun)
			return FALSE;
		if (d->pal)
			gdi.SelectPalette(hdc, d->pal,
					  (flags & DDF_BACKGROUNDPAL) != 0);
		return gdi.StretchBlt(hdc, x_dst, y_dst, d->dx_dst, d->dy_dst,
				      d->mem, 0, 0, d->dx_src, d->dy_src,
				      SRCCOPY);
	}
	if (!bi)
		return FALSE;
	/* -1 for the source's size, then the destination's from it. */
	if (dx_src == -1 && dy_src == -1) {
		dx_src = bi->biWidth;
		dy_src = abs_height(bi);
	}
	if (dx_dst == -1 && dy_dst == -1) {
		dx_dst = dx_src;
		dy_dst = dy_src;
	}
	if (!bits)
		bits = (BYTE *)bi + fmt_size(bi);

	/* Anything changed from what was begun means beginning again. */
	int reopen = !d->begun;
	if (!reopen && !(flags & DDF_SAME_HDC) && hdc != d->hdc)
		reopen = 1;
	if (!reopen && !(flags & DDF_SAME_DRAW) &&
	    (bi != d->in && memcmp(bi, d->in, sizeof(*bi))))
		reopen = 1;
	if (!reopen && !(flags & DDF_SAME_DRAW) &&
	    (dx_src != d->dx_src || dy_src != d->dy_src ||
	     dx_dst != d->dx_dst || dy_dst != d->dy_dst))
		reopen = 1;
	if (reopen) {
		HPALETTE pal = d->pal, own = d->own_pal;
		d->own_pal = NULL;	/* kept over the new begin */
		if (!begin(d, hdc, dx_dst, dy_dst, bi, dx_src, dy_src)) {
			if (own)
				gdi.DeleteObject(own);
			return FALSE;
		}
		d->pal = pal;
		d->own_pal = own;
	}

	if (!(flags & DDF_UPDATE)) {
		if (d->hic) {
			DWORD f = flags & DDF_NOTKEYFRAME ?
				  ICDECOMPRESS_NOTKEYFRAME : 0;
			if (flags & DDF_HURRYUP)
				f |= ICDECOMPRESS_HURRYUP;
			DWORD r = ic_decompress(d->hic, f, bi, bits, d->out,
						d->bits);
			if ((LONG)r < 0) {
				TRACE("DrawDibDraw: decompress failed (%ld)",
				      (long)(LONG)r);
				return FALSE;
			}
			if (r == ICERR_DONTDRAW)
				return TRUE;	/* nothing new to show */
		} else {
			memcpy(d->bits, bits, rgb_bytes(d->out));
		}
	}
	if (flags & DDF_DONTDRAW)
		return TRUE;

	if (d->pal)
		gdi.SelectPalette(hdc, d->pal,
				  (flags & DDF_BACKGROUNDPAL) != 0);
	return gdi.StretchBlt(hdc, x_dst, y_dst, dx_dst, dy_dst, d->mem, x_src,
			      y_src, dx_src, dy_src, SRCCOPY);
}

WINFN(LPVOID) vfw_DrawDibGetBuffer(HDRAWDIB h, LPBITMAPINFOHEADER bi,
				   DWORD size, DWORD flags)
{
	struct hdd *d = get(h);

	if (!d || !d->begun)
		return NULL;
	if (bi) {
		size_t n = fmt_size(d->out);
		memcpy(bi, d->out, size < n ? size : n);
	}
	return d->bits;
}

WINFN(HPALETTE) vfw_DrawDibGetPalette(HDRAWDIB h)
{
	struct hdd *d = get(h);

	return d ? d->pal : NULL;
}

WINFN(BOOL) vfw_DrawDibSetPalette(HDRAWDIB h, HPALETTE pal)
{
	struct hdd *d = get(h);

	if (!d)
		return FALSE;
	d->pal = pal;
	if (d->begun && pal) {
		gdi.SelectPalette(d->hdc, pal, FALSE);
		gdi.RealizePalette(d->hdc);
	}
	return TRUE;
}

WINFN(UINT) vfw_DrawDibRealize(HDRAWDIB h, HDC hdc, BOOL background)
{
	struct hdd *d = get(h);

	if (!d || !d->begun)
		return 0;
	if (!d->pal)
		d->pal = d->own_pal = gdi.CreateHalftonePalette(hdc);
	gdi.SelectPalette(hdc, d->pal, background);
	return gdi.RealizePalette(hdc);
}

/* New colours for a palettised buffer, from entry start on. */
WINFN(BOOL) vfw_DrawDibChangePalette(HDRAWDIB h, int start, int len,
				     LPPALETTEENTRY pe)
{
	struct hdd *d = get(h);

	if (!d || !d->begun || start < 0 || len < 0 || (len && !pe))
		return FALSE;
	DWORD n = d->out->biBitCount <= 8 ? num_colours(d->out) : 0;
	if ((DWORD)start >= n)
		return len == 0;
	if ((DWORD)(start + len) > n)
		len = (int)(n - start);
	RGBQUAD *q = (RGBQUAD *)((BYTE *)d->out + d->out->biSize) + start;
	for (int i = 0; i < len; i++) {
		q[i].rgbRed = pe[i].peRed;
		q[i].rgbGreen = pe[i].peGreen;
		q[i].rgbBlue = pe[i].peBlue;
		q[i].rgbReserved = 0;
	}
	gdi.SetDIBColorTable(d->mem, (UINT)start, (UINT)len, q);
	return TRUE;
}

WINFN(BOOL) vfw_DrawDibStart(HDRAWDIB h, DWORD rate)
{
	return get(h) != NULL;
}

WINFN(BOOL) vfw_DrawDibStop(HDRAWDIB h)
{
	return get(h) != NULL;
}

/* Timing is only kept by debug builds of msvfw32, as in Wine. */
WINFN(BOOL) vfw_DrawDibTime(HDRAWDIB h, LPDRAWDIBTIME t)
{
	return FALSE;
}

WINFN(LRESULT) vfw_DrawDibProfileDisplay(LPBITMAPINFOHEADER bi)
{
	return PD_CAN_DRAW_DIB | PD_CAN_STRETCHDIB | PD_STRETCHDIB_1_1_OK |
	       PD_STRETCHDIB_1_2_OK | PD_STRETCHDIB_1_N_OK;
}

/*
 * ICDrawBegin and ICDraw (cdecl), after Wine: the arguments packed into
 * an ICDRAWBEGIN or ICDRAW for a codec that draws for itself.
 */
static DWORD STUB_CDECL ic_draw_begin(HIC hic, DWORD flags, HPALETTE pal, HWND hwnd,
			       HDC hdc, int x_dst, int y_dst, int dx_dst,
			       int dy_dst, LPBITMAPINFOHEADER bi, int x_src,
			       int y_src, int dx_src, int dy_src, DWORD rate,
			       DWORD scale)
{
	ICDRAWBEGIN b = {
		flags, pal, hwnd, hdc, x_dst, y_dst, dx_dst, dy_dst, bi,
		x_src, y_src, dx_src, dy_src, rate, scale
	};

	return (DWORD)ic_send(hic, ICM_DRAW_BEGIN, (DWORD_PTR)&b, sizeof(b));
}

static DWORD STUB_CDECL ic_draw(HIC hic, DWORD flags, LPVOID fmt, LPVOID data,
			 DWORD size, LONG time)
{
	ICDRAW d = { flags, fmt, data, size, time };

	return (DWORD)ic_send(hic, ICM_DRAW, (DWORD_PTR)&d, sizeof(d));
}

const struct builtin_export vfw_drawdib_exports[] = {
	{ "DrawDibBegin", 0, (void *)vfw_DrawDibBegin },
	{ "DrawDibChangePalette", 0, (void *)vfw_DrawDibChangePalette },
	{ "DrawDibClose", 0, (void *)vfw_DrawDibClose },
	{ "DrawDibDraw", 0, (void *)vfw_DrawDibDraw },
	{ "DrawDibEnd", 0, (void *)vfw_DrawDibEnd },
	{ "DrawDibGetBuffer", 0, (void *)vfw_DrawDibGetBuffer },
	{ "DrawDibGetPalette", 0, (void *)vfw_DrawDibGetPalette },
	{ "DrawDibOpen", 0, (void *)vfw_DrawDibOpen },
	{ "DrawDibProfileDisplay", 0, (void *)vfw_DrawDibProfileDisplay },
	{ "DrawDibRealize", 0, (void *)vfw_DrawDibRealize },
	{ "DrawDibSetPalette", 0, (void *)vfw_DrawDibSetPalette },
	{ "DrawDibStart", 0, (void *)vfw_DrawDibStart },
	{ "DrawDibStop", 0, (void *)vfw_DrawDibStop },
	{ "DrawDibTime", 0, (void *)vfw_DrawDibTime },
	{ "ICDraw", 0, (void *)ic_draw },
	{ "ICDrawBegin", 0, (void *)ic_draw_begin },
	BUILTIN_END
};
