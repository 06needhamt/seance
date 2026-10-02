/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * comctl32: image lists.
 *
 * An image list keeps its images side by side in one row of a 32-bit
 * top-down DIB section, whatever colour depth it was created with, and
 * their masks in a matching monochrome DIB section (1 = transparent), so
 * ImageList_GetImageInfo can hand out real bitmaps. Images come in and go
 * out through gdi32's BitBlt, which converts formats; where the mask is
 * transparent the image is kept black, so drawing is the classic mask
 * SRCAND then image SRCPAINT. Blending, alpha and rgbFg are ignored.
 *
 * Icons are handles with nothing behind them in this user32, so an icon
 * added is a blank, fully transparent image, and ImageList_GetIcon hands
 * back a new such handle. There is nothing to drag, and no streams.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "win32/comctl.h"
#include "win32/internal.h"

#define IL_MAGIC 0x4c4d4948	/* "HIML" */

struct _IMAGELIST {
	DWORD magic;
	int cx, cy;
	UINT flags;
	int count, max, grow;
	HBITMAP image, mask;	/* mask is NULL without ILC_MASK */
	DWORD *bits;		/* image: max * cx by cy, 32 bpp */
	BYTE *mbits;		/* mask: 1 bpp rows of mstride bytes */
	int mstride;
	HDC dc, mdc;		/* memory DCs holding image and mask */
	HGDIOBJ dc_old, mdc_old;
	COLORREF bk;
	int overlay[15];
};

/* gdi32 and user32 entry points, looked up once. */
static struct {
	HDC (WINAPI *CreateCompatibleDC)(HDC);
	BOOL (WINAPI *DeleteDC)(HDC);
	HGDIOBJ (WINAPI *SelectObject)(HDC, HGDIOBJ);
	BOOL (WINAPI *DeleteObject)(HGDIOBJ);
	HBITMAP (WINAPI *CreateDIBSection)(HDC, const BITMAPINFO *, UINT,
					   void **, HANDLE, DWORD);
	BOOL (WINAPI *BitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);
	BOOL (WINAPI *PatBlt)(HDC, int, int, int, int, DWORD);
	HBRUSH (WINAPI *CreateSolidBrush)(COLORREF);
	int (WINAPI *GetObjectW)(HGDIOBJ, int, void *);
	COLORREF (WINAPI *GetPixel)(HDC, int, int);
	HICON (WINAPI *LoadIconW)(HINSTANCE, LPCWSTR);
	HANDLE (WINAPI *LoadImageA)(HINSTANCE, LPCSTR, UINT, int, int, UINT);
	HANDLE (WINAPI *LoadImageW)(HINSTANCE, LPCWSTR, UINT, int, int, UINT);
} api;

static pthread_once_t api_once = PTHREAD_ONCE_INIT;

static void api_init(void)
{
	api.CreateCompatibleDC = GDI32(CreateCompatibleDC);
	api.DeleteDC = GDI32(DeleteDC);
	api.SelectObject = GDI32(SelectObject);
	api.DeleteObject = GDI32(DeleteObject);
	api.CreateDIBSection = GDI32(CreateDIBSection);
	api.BitBlt = GDI32(BitBlt);
	api.PatBlt = GDI32(PatBlt);
	api.CreateSolidBrush = GDI32(CreateSolidBrush);
	api.GetObjectW = GDI32(GetObjectW);
	api.GetPixel = GDI32(GetPixel);
	api.LoadIconW = USER32(LoadIconW);
	api.LoadImageA = USER32(LoadImageA);
	api.LoadImageW = USER32(LoadImageW);
}

static HIMAGELIST valid(HIMAGELIST h)
{
	return h && h->magic == IL_MAGIC ? h : NULL;
}

static int has_mask(HIMAGELIST h)
{
	return h->mask != NULL;
}

/* Mask bits: 1 = transparent. x is across the whole strip. */
static int mask_get(HIMAGELIST h, int x, int y)
{
	return h->mbits[y * h->mstride + (x >> 3)] >> (7 - (x & 7)) & 1;
}

static void mask_set(HIMAGELIST h, int x, int y, int on)
{
	BYTE *b = &h->mbits[y * h->mstride + (x >> 3)];
	BYTE bit = 0x80 >> (x & 7);
	*b = on ? *b | bit : *b & ~bit;
}

static DWORD *pixel(HIMAGELIST h, int x, int y)
{
	return &h->bits[(size_t)y * h->max * h->cx + x];
}

/* Two DIB sections for max images, their bits, and DCs holding them. */
static int alloc_strip(HIMAGELIST h, int max, HBITMAP *image, DWORD **bits,
		       HBITMAP *mask, BYTE **mbits)
{
	struct {
		BITMAPINFOHEADER h;
		RGBQUAD c[2];
	} bi;

	memset(&bi, 0, sizeof(bi));
	bi.h.biSize = sizeof(bi.h);
	bi.h.biWidth = max * h->cx;
	bi.h.biHeight = -h->cy;
	bi.h.biPlanes = 1;
	bi.h.biBitCount = 32;
	*image = api.CreateDIBSection(NULL, (BITMAPINFO *)&bi, DIB_RGB_COLORS,
				      (void **)bits, NULL, 0);
	*mask = NULL;
	if (!*image)
		return 0;
	if (!(h->flags & ILC_MASK))
		return 1;
	bi.h.biBitCount = 1;
	bi.c[1].rgbRed = bi.c[1].rgbGreen = bi.c[1].rgbBlue = 0xff;
	*mask = api.CreateDIBSection(NULL, (BITMAPINFO *)&bi, DIB_RGB_COLORS,
				     (void **)mbits, NULL, 0);
	if (!*mask) {
		api.DeleteObject(*image);
		*image = NULL;
		return 0;
	}
	return 1;
}

/* Room for n images, in whole grow steps; keeps what is there. */
static int reserve(HIMAGELIST h, int n)
{
	if (n <= h->max)
		return 1;
	int max = h->max;
	while (max < n)
		max += h->grow;
	if ((long long)max * h->cx > 0x7fff0000 / 4)
		return 0;

	HBITMAP image, mask;
	DWORD *bits;
	BYTE *mbits = NULL;
	if (!alloc_strip(h, max, &image, &bits, &mask, &mbits))
		return 0;
	int mstride = ((max * h->cx + 31) / 32) * 4;
	for (int y = 0; y < h->cy; y++) {
		memcpy(bits + (size_t)y * max * h->cx,
		       h->bits + (size_t)y * h->max * h->cx,
		       (size_t)h->count * h->cx * 4);
		if (mask)
			memcpy(mbits + y * mstride, h->mbits + y * h->mstride,
			       h->mstride);
	}
	api.SelectObject(h->dc, image);
	api.DeleteObject(h->image);
	h->image = image;
	h->bits = bits;
	if (mask) {
		api.SelectObject(h->mdc, mask);
		api.DeleteObject(h->mask);
		h->mask = mask;
		h->mbits = mbits;
		h->mstride = mstride;
	}
	h->max = max;
	return 1;
}

/* Makes image i blank: black, and transparent (or opaque) in the mask. */
static void blank(HIMAGELIST h, int i, int transparent)
{
	for (int y = 0; y < h->cy; y++) {
		memset(pixel(h, i * h->cx, y), 0, h->cx * 4);
		for (int x = 0; has_mask(h) && x < h->cx; x++)
			mask_set(h, i * h->cx + x, y, transparent);
	}
}

/* Keeps the image black where the mask is transparent. */
static void clear_under_mask(HIMAGELIST h, int first, int n)
{
	if (!has_mask(h))
		return;
	for (int y = 0; y < h->cy; y++)
		for (int x = first * h->cx; x < (first + n) * h->cx; x++)
			if (mask_get(h, x, y))
				*pixel(h, x, y) = 0;
}

/* Copies image src of s over image dst of d (the same sizes). */
static void copy_image(HIMAGELIST d, int dst, HIMAGELIST s, int src)
{
	for (int y = 0; y < d->cy; y++) {
		memmove(pixel(d, dst * d->cx, y), pixel(s, src * s->cx, y),
			d->cx * 4);
		for (int x = 0; has_mask(d) && x < d->cx; x++)
			mask_set(d, dst * d->cx + x, y,
				 has_mask(s) ? mask_get(s, src * s->cx + x, y) :
					       0);
	}
}

static void destroy(HIMAGELIST h)
{
	if (h->dc) {
		api.SelectObject(h->dc, h->dc_old);
		api.DeleteDC(h->dc);
	}
	if (h->mdc) {
		api.SelectObject(h->mdc, h->mdc_old);
		api.DeleteDC(h->mdc);
	}
	if (h->image)
		api.DeleteObject(h->image);
	if (h->mask)
		api.DeleteObject(h->mask);
	h->magic = 0;
	comctl_free(h);
}

WINFN(HIMAGELIST) il_ImageList_Create(INT cx, INT cy, UINT flags, INT initial,
				      INT grow)
{
	pthread_once(&api_once, api_init);
	if (cx <= 0 || cy <= 0 || initial < 0)
		return NULL;
	HIMAGELIST h = comctl_alloc(sizeof(*h));
	if (!h)
		return NULL;
	h->magic = IL_MAGIC;
	h->cx = cx;
	h->cy = cy;
	h->flags = flags;
	h->grow = grow > 4 ? grow : 4;
	h->max = initial > 0 ? initial : 1;
	h->bk = CLR_NONE;
	for (int i = 0; i < 15; i++)
		h->overlay[i] = -1;
	if (!alloc_strip(h, h->max, &h->image, &h->bits, &h->mask, &h->mbits) ||
	    !(h->dc = api.CreateCompatibleDC(NULL)) ||
	    (h->mask && !(h->mdc = api.CreateCompatibleDC(NULL)))) {
		destroy(h);
		return NULL;
	}
	h->mstride = ((h->max * cx + 31) / 32) * 4;
	h->dc_old = api.SelectObject(h->dc, h->image);
	if (h->mask)
		h->mdc_old = api.SelectObject(h->mdc, h->mask);
	return h;
}

WINFN(BOOL) il_ImageList_Destroy(HIMAGELIST h)
{
	if (!valid(h))
		return FALSE;
	destroy(h);
	return TRUE;
}

WINFN(INT) il_ImageList_GetImageCount(HIMAGELIST h)
{
	return valid(h) ? h->count : 0;
}

/* New images are blank and opaque. */
WINFN(BOOL) il_ImageList_SetImageCount(HIMAGELIST h, UINT n)
{
	if (!valid(h) || n > 0x7fffffff || !reserve(h, (int)n))
		return FALSE;
	for (int i = h->count; i < (int)n; i++)
		blank(h, i, 0);
	h->count = (int)n;
	return TRUE;
}

WINFN(BOOL) il_ImageList_GetIconSize(HIMAGELIST h, INT *cx, INT *cy)
{
	if (!valid(h) || !cx || !cy)
		return FALSE;
	*cx = h->cx;
	*cy = h->cy;
	return TRUE;
}

/* A new size drops every image. */
WINFN(BOOL) il_ImageList_SetIconSize(HIMAGELIST h, INT cx, INT cy)
{
	if (!valid(h) || cx <= 0 || cy <= 0)
		return FALSE;
	HIMAGELIST n = il_ImageList_Create(cx, cy, h->flags, h->max, h->grow);
	if (!n)
		return FALSE;
	/* Swap the strips, keeping the handle. */
	struct _IMAGELIST t = *h;
	*h = *n;
	h->bk = t.bk;
	*n = t;
	n->magic = IL_MAGIC;
	destroy(n);
	return TRUE;
}

WINFN(COLORREF) il_ImageList_GetBkColor(HIMAGELIST h)
{
	return valid(h) ? h->bk : CLR_NONE;
}

WINFN(COLORREF) il_ImageList_SetBkColor(HIMAGELIST h, COLORREF bk)
{
	if (!valid(h))
		return CLR_NONE;
	COLORREF old = h->bk;
	h->bk = bk;
	return old;
}

WINFN(DWORD) il_ImageList_GetFlags(HIMAGELIST h)
{
	return valid(h) ? h->flags : 0;
}

/* Only the colour depth may change; the images are kept at 32 bits. */
WINFN(DWORD) il_ImageList_SetFlags(HIMAGELIST h, DWORD flags)
{
	if (!valid(h) || (flags & ILC_MASK) != (h->flags & ILC_MASK))
		return FALSE;
	h->flags = flags;
	return TRUE;
}

/* The size of a bitmap, or 0. */
static int bitmap_size(HBITMAP bmp, int *w, int *hgt)
{
	BITMAP bm;

	if (!bmp || api.GetObjectW(bmp, sizeof(bm), &bm) != sizeof(bm))
		return 0;
	*w = bm.bmWidth;
	*hgt = bm.bmHeight;
	return 1;
}

/*
 * Puts the images of bmp (as many as fit its width, at least one) at i
 * on; with mask, their masks too, else they are opaque. Returns how many.
 */
static int put_bitmap(HIMAGELIST h, int i, HBITMAP bmp, HBITMAP mask,
		      int *grow_count)
{
	int w, hgt;

	if (!bitmap_size(bmp, &w, &hgt))
		return 0;
	int n = w / h->cx;
	if (n < 1)
		n = 1;
	if (grow_count && !reserve(h, i + n))
		return 0;
	if (!grow_count && i + n > h->count)
		n = h->count - i;

	HDC src = api.CreateCompatibleDC(NULL);
	if (!src)
		return 0;
	HGDIOBJ old = api.SelectObject(src, bmp);
	api.BitBlt(h->dc, i * h->cx, 0, n * h->cx, h->cy, src, 0, 0, SRCCOPY);
	if (has_mask(h)) {
		if (mask) {
			api.SelectObject(src, mask);
			api.BitBlt(h->mdc, i * h->cx, 0, n * h->cx, h->cy, src,
				   0, 0, SRCCOPY);
		} else {
			api.PatBlt(h->mdc, i * h->cx, 0, n * h->cx, h->cy,
				   BLACKNESS);
		}
	}
	api.SelectObject(src, old);
	api.DeleteDC(src);
	if (grow_count && i + n > h->count)
		*grow_count = i + n;
	clear_under_mask(h, i, n);
	return n;
}

WINFN(INT) il_ImageList_Add(HIMAGELIST h, HBITMAP image, HBITMAP mask)
{
	if (!valid(h))
		return -1;
	int first = h->count, count = h->count;
	if (!put_bitmap(h, first, image, mask, &count))
		return -1;
	h->count = count;
	return first;
}

/* The mask is where the bitmap has colour mask (CLR_DEFAULT: its corner's). */
WINFN(INT) il_ImageList_AddMasked(HIMAGELIST h, HBITMAP image, COLORREF mask)
{
	if (!valid(h))
		return -1;
	int first = h->count, count = h->count;
	int n = put_bitmap(h, first, image, NULL, &count);
	if (!n)
		return -1;
	h->count = count;
	if (!has_mask(h))
		return first;
	if (mask == CLR_DEFAULT)
		mask = api.GetPixel(h->dc, first * h->cx, 0);
	DWORD want = (GetRValue(mask) << 16) | (GetGValue(mask) << 8) |
		     GetBValue(mask);
	for (int y = 0; y < h->cy; y++)
		for (int x = first * h->cx; x < (first + n) * h->cx; x++) {
			int clear = (*pixel(h, x, y) & 0xffffff) == want;
			mask_set(h, x, y, clear);
			if (clear)
				*pixel(h, x, y) = 0;
		}
	return first;
}

WINFN(BOOL) il_ImageList_Replace(HIMAGELIST h, INT i, HBITMAP image,
				 HBITMAP mask)
{
	if (!valid(h) || i < 0 || i >= h->count)
		return FALSE;
	return put_bitmap(h, i, image, mask, NULL) > 0;
}

/* An icon is a blank, transparent image here (see the top). */
WINFN(INT) il_ImageList_ReplaceIcon(HIMAGELIST h, INT i, HICON icon)
{
	if (!valid(h) || !icon || i < -1 || i >= h->count)
		return -1;
	if (i == -1) {
		if (!reserve(h, h->count + 1))
			return -1;
		i = h->count++;
	}
	blank(h, i, 1);
	return i;
}

WINFN(INT) il_ImageList_AddIcon(HIMAGELIST h, HICON icon)
{
	return il_ImageList_ReplaceIcon(h, -1, icon);
}

WINFN(HICON) il_ImageList_GetIcon(HIMAGELIST h, INT i, UINT flags)
{
	if (!valid(h) || i < 0 || i >= h->count)
		return NULL;
	return api.LoadIconW(NULL, (LPCWSTR)(ULONG_PTR)32512);	/* IDI_APPLICATION */
}

/* -1 removes them all. */
WINFN(BOOL) il_ImageList_Remove(HIMAGELIST h, INT i)
{
	if (!valid(h) || i < -1 || i >= h->count)
		return FALSE;
	if (i == -1) {
		h->count = 0;
		for (int j = 0; j < 15; j++)
			h->overlay[j] = -1;
		return TRUE;
	}
	for (int j = i; j + 1 < h->count; j++)
		copy_image(h, j, h, j + 1);
	h->count--;
	for (int j = 0; j < 15; j++)
		if (h->overlay[j] == i)
			h->overlay[j] = -1;
		else if (h->overlay[j] > i)
			h->overlay[j]--;
	return TRUE;
}

WINFN(BOOL) il_ImageList_SetOverlayImage(HIMAGELIST h, INT image, INT overlay)
{
	if (!valid(h) || overlay < 1 || overlay > 15 || image < -1 ||
	    image >= h->count)
		return FALSE;
	h->overlay[overlay - 1] = image;
	return TRUE;
}

WINFN(BOOL) il_ImageList_GetImageRect(HIMAGELIST h, INT i, LPRECT rc)
{
	if (!valid(h) || !rc || i < 0 || i >= h->count)
		return FALSE;
	rc->left = i * h->cx;
	rc->top = 0;
	rc->right = rc->left + h->cx;
	rc->bottom = h->cy;
	return TRUE;
}

WINFN(BOOL) il_ImageList_GetImageInfo(HIMAGELIST h, INT i, IMAGEINFO *info)
{
	if (!valid(h) || !info || i < 0 || i >= h->count)
		return FALSE;
	info->hbmImage = h->image;
	info->hbmMask = h->mask;
	return il_ImageList_GetImageRect(h, i, &info->rcImage);
}

/* Draws image i at (x, y) on dc: part of it, from (sx, sy), cx by cy. */
static void draw(HIMAGELIST h, int i, HDC dc, int x, int y, int cx, int cy,
		 int sx, int sy, COLORREF bk, UINT style, DWORD rop)
{
	int ix = i * h->cx + sx;

	if (style & ILD_MASK) {
		if (has_mask(h))
			api.BitBlt(dc, x, y, cx, cy, h->mdc, ix, sy, SRCCOPY);
		else
			api.PatBlt(dc, x, y, cx, cy, BLACKNESS);
		return;
	}
	if (!(style & ILD_ROP))
		rop = SRCCOPY;
	if (bk == CLR_DEFAULT)
		bk = h->bk;
	int transparent = bk == CLR_NONE || (style & ILD_TRANSPARENT);
	if (!has_mask(h) || (style & ILD_IMAGE) || rop != SRCCOPY) {
		api.BitBlt(dc, x, y, cx, cy, h->dc, ix, sy, rop);
		return;
	}
	if (!transparent) {
		HBRUSH brush = api.CreateSolidBrush(bk);
		HGDIOBJ old = api.SelectObject(dc, brush);
		api.PatBlt(dc, x, y, cx, cy, PATCOPY);
		api.SelectObject(dc, old);
		api.DeleteObject(brush);
	}
	api.BitBlt(dc, x, y, cx, cy, h->mdc, ix, sy, SRCAND);
	api.BitBlt(dc, x, y, cx, cy, h->dc, ix, sy, SRCPAINT);
}

static BOOL draw_params(const IMAGELISTDRAWPARAMS *p)
{
	HIMAGELIST h;

	if (!p || p->cbSize < VFW_IMAGELISTDRAWPARAMS_V3_SIZE ||
	    !(h = valid(p->himl)) || p->i < 0 || p->i >= h->count || !p->hdcDst)
		return FALSE;
	int cx = p->cx > 0 ? p->cx : h->cx - p->xBitmap;
	int cy = p->cy > 0 ? p->cy : h->cy - p->yBitmap;
	if (cx > h->cx - p->xBitmap)
		cx = h->cx - p->xBitmap;
	if (cy > h->cy - p->yBitmap)
		cy = h->cy - p->yBitmap;
	if (cx <= 0 || cy <= 0 || p->xBitmap < 0 || p->yBitmap < 0)
		return FALSE;
	draw(h, p->i, p->hdcDst, p->x, p->y, cx, cy, p->xBitmap, p->yBitmap,
	     p->rgbBk, p->fStyle, p->dwRop);

	int ov = (p->fStyle & ILD_OVERLAYMASK) >> 8;
	if (ov && !(p->fStyle & (ILD_MASK | ILD_IMAGE)) &&
	    h->overlay[ov - 1] >= 0 && h->overlay[ov - 1] < h->count)
		draw(h, h->overlay[ov - 1], p->hdcDst, p->x, p->y, cx, cy,
		     p->xBitmap, p->yBitmap, CLR_NONE, ILD_TRANSPARENT, 0);
	return TRUE;
}

WINFN(BOOL) il_ImageList_DrawIndirect(IMAGELISTDRAWPARAMS *p)
{
	return draw_params(p);
}

WINFN(BOOL) il_ImageList_DrawEx(HIMAGELIST h, INT i, HDC dc, INT x, INT y,
				INT cx, INT cy, COLORREF bk, COLORREF fg,
				UINT style)
{
	IMAGELISTDRAWPARAMS p;

	memset(&p, 0, sizeof(p));
	p.cbSize = sizeof(p);
	p.himl = h;
	p.i = i;
	p.hdcDst = dc;
	p.x = x;
	p.y = y;
	p.cx = cx;
	p.cy = cy;
	p.rgbBk = bk;
	p.rgbFg = fg;
	p.fStyle = style;
	return draw_params(&p);
}

WINFN(BOOL) il_ImageList_Draw(HIMAGELIST h, INT i, HDC dc, INT x, INT y,
			      UINT style)
{
	return il_ImageList_DrawEx(h, i, dc, x, y, 0, 0, CLR_DEFAULT,
				   CLR_DEFAULT, style);
}

WINFN(HIMAGELIST) il_ImageList_Duplicate(HIMAGELIST h)
{
	if (!valid(h))
		return NULL;
	HIMAGELIST n = il_ImageList_Create(h->cx, h->cy, h->flags,
					   h->count ? h->count : 1, h->grow);
	if (!n)
		return NULL;
	for (int i = 0; i < h->count; i++)
		copy_image(n, i, h, i);
	n->count = h->count;
	n->bk = h->bk;
	memcpy(n->overlay, h->overlay, sizeof(n->overlay));
	return n;
}

/* Within one list: ILCF_MOVE copies the image over, ILCF_SWAP swaps. */
WINFN(BOOL) il_ImageList_Copy(HIMAGELIST dst, INT idst, HIMAGELIST src,
			      INT isrc, UINT flags)
{
	if (!valid(dst) || !valid(src) || dst->cx != src->cx ||
	    dst->cy != src->cy || idst < 0 || idst >= dst->count || isrc < 0 ||
	    isrc >= src->count)
		return FALSE;
	if (!(flags & ILCF_SWAP)) {
		copy_image(dst, idst, src, isrc);
		return TRUE;
	}
	/* Swap through a scratch list of one image. */
	HIMAGELIST t = il_ImageList_Create(dst->cx, dst->cy,
					   dst->flags | ILC_MASK, 1, 1);
	if (!t)
		return FALSE;
	t->count = 1;
	copy_image(t, 0, dst, idst);
	copy_image(dst, idst, src, isrc);
	copy_image(src, isrc, t, 0);
	destroy(t);
	return TRUE;
}

/* A new list of one image: i2 of h2 over i1 of h1, offset by (dx, dy). */
WINFN(HIMAGELIST) il_ImageList_Merge(HIMAGELIST h1, INT i1, HIMAGELIST h2,
				     INT i2, INT dx, INT dy)
{
	if (!valid(h1) || !valid(h2) || i1 < 0 || i1 >= h1->count || i2 < 0 ||
	    i2 >= h2->count)
		return NULL;
	int x1 = dx < 0 ? -dx : 0, x2 = dx > 0 ? dx : 0;
	int y1 = dy < 0 ? -dy : 0, y2 = dy > 0 ? dy : 0;
	int cx = x1 + h1->cx > x2 + h2->cx ? x1 + h1->cx : x2 + h2->cx;
	int cy = y1 + h1->cy > y2 + h2->cy ? y1 + h1->cy : y2 + h2->cy;
	HIMAGELIST m = il_ImageList_Create(cx, cy, ILC_MASK | (h1->flags & 0xfe),
					   1, 1);
	if (!m)
		return NULL;
	m->count = 1;
	blank(m, 0, 1);
	const struct { HIMAGELIST h; int i, x, y; } src[2] = {
		{ h1, i1, x1, y1 }, { h2, i2, x2, y2 },
	};
	for (int k = 0; k < 2; k++) {
		HIMAGELIST s = src[k].h;
		for (int y = 0; y < s->cy; y++)
			for (int x = 0; x < s->cx; x++) {
				int sx = src[k].i * s->cx + x;
				if (has_mask(s) && mask_get(s, sx, y))
					continue;
				*pixel(m, src[k].x + x, src[k].y + y) =
					*pixel(s, sx, y);
				mask_set(m, src[k].x + x, src[k].y + y, 0);
			}
	}
	return m;
}

static HIMAGELIST load_image(HINSTANCE inst, const void *name, int wide,
			     INT cx, INT grow, COLORREF mask, UINT type,
			     UINT flags)
{
	pthread_once(&api_once, api_init);
	HANDLE img = wide ? api.LoadImageW(inst, name, type, 0, 0, flags) :
			    api.LoadImageA(inst, name, type, 0, 0, flags);
	if (!img)
		return NULL;
	HIMAGELIST h = NULL;
	UINT ilc = ILC_COLOR32 | (mask != CLR_NONE ? ILC_MASK : 0);
	if (type == IMAGE_BITMAP) {
		int w, hgt;
		if (bitmap_size(img, &w, &hgt)) {
			if (cx <= 0)
				cx = hgt;
			int n = w / cx > 0 ? w / cx : 1;
			if ((h = il_ImageList_Create(cx, hgt, ilc, n, grow))) {
				int r = mask != CLR_NONE ?
					il_ImageList_AddMasked(h, img, mask) :
					il_ImageList_Add(h, img, NULL);
				if (r < 0) {
					destroy(h);
					h = NULL;
				}
			}
		}
		api.DeleteObject(img);
	} else if ((h = il_ImageList_Create(cx > 0 ? cx : 32, cx > 0 ? cx : 32,
					    ilc, 1, grow))) {
		il_ImageList_AddIcon(h, img);
	}
	return h;
}

WINFN(HIMAGELIST) il_ImageList_LoadImageA(HINSTANCE inst, LPCSTR name, INT cx,
					  INT grow, COLORREF mask, UINT type,
					  UINT flags)
{
	return load_image(inst, name, 0, cx, grow, mask, type, flags);
}

WINFN(HIMAGELIST) il_ImageList_LoadImageW(HINSTANCE inst, LPCWSTR name, INT cx,
					  INT grow, COLORREF mask, UINT type,
					  UINT flags)
{
	return load_image(inst, name, 1, cx, grow, mask, type, flags);
}

/* Dragging: there is no mouse, so a drag only remembers its image. */

static HIMAGELIST drag_list;
static POINT drag_hotspot;

WINFN(BOOL) il_ImageList_BeginDrag(HIMAGELIST h, INT i, INT dx, INT dy)
{
	if (!valid(h) || i < 0 || i >= h->count)
		return FALSE;
	if (drag_list)
		destroy(drag_list);
	if (!(drag_list = il_ImageList_Create(h->cx, h->cy, h->flags | ILC_MASK,
					      1, 1)))
		return FALSE;
	drag_list->count = 1;
	copy_image(drag_list, 0, h, i);
	drag_hotspot.x = dx;
	drag_hotspot.y = dy;
	return TRUE;
}

WINFN(void) il_ImageList_EndDrag(void)
{
	if (drag_list)
		destroy(drag_list);
	drag_list = NULL;
}

WINFN(HIMAGELIST) il_ImageList_GetDragImage(POINT *pt, POINT *hotspot)
{
	if (drag_list) {
		if (pt)
			pt->x = pt->y = 0;
		if (hotspot)
			*hotspot = drag_hotspot;
	}
	return drag_list;
}

WINFN(BOOL) il_ImageList_DragEnter(HWND lock, INT x, INT y) { return drag_list != NULL; }
WINFN(BOOL) il_ImageList_DragLeave(HWND lock) { return TRUE; }
WINFN(BOOL) il_ImageList_DragMove(INT x, INT y) { return drag_list != NULL; }
WINFN(BOOL) il_ImageList_DragShowNolock(BOOL show) { return TRUE; }

WINFN(BOOL) il_ImageList_SetDragCursorImage(HIMAGELIST h, INT i, INT dx, INT dy)
{
	return valid(h) && i >= 0 && i < h->count && drag_list;
}

/* No IStream persistence, and nothing to filter or recolour. */

WINFN(HIMAGELIST) il_ImageList_Read(void *stream) { return NULL; }
WINFN(BOOL) il_ImageList_Write(HIMAGELIST h, void *stream) { return FALSE; }
WINFN(BOOL) il_ImageList_SetFilter(HIMAGELIST h, INT i, DWORD filter) { return FALSE; }

WINFN(UINT) il_ImageList_SetColorTable(HIMAGELIST h, UINT start, UINT n,
				       const RGBQUAD *colors)
{
	return 0;
}

WINFN(HRESULT) il_HIMAGELIST_QueryInterface(HIMAGELIST h, REFIID riid,
					    void **out)
{
	if (out)
		*out = NULL;
	return E_NOINTERFACE;
}

#define E(name) { #name, 0, (void *)il_##name }

const struct builtin_export comctl_imagelist_exports[] = {
	E(HIMAGELIST_QueryInterface),
	E(ImageList_Add),
	E(ImageList_AddIcon),
	E(ImageList_AddMasked),
	E(ImageList_BeginDrag),
	E(ImageList_Copy),
	E(ImageList_Create),
	E(ImageList_Destroy),
	E(ImageList_DragEnter),
	E(ImageList_DragLeave),
	E(ImageList_DragMove),
	E(ImageList_DragShowNolock),
	E(ImageList_Draw),
	E(ImageList_DrawEx),
	E(ImageList_DrawIndirect),
	E(ImageList_Duplicate),
	E(ImageList_EndDrag),
	E(ImageList_GetBkColor),
	E(ImageList_GetDragImage),
	E(ImageList_GetFlags),
	E(ImageList_GetIcon),
	E(ImageList_GetIconSize),
	E(ImageList_GetImageCount),
	E(ImageList_GetImageInfo),
	E(ImageList_GetImageRect),
	{ "ImageList_LoadImage", 0, (void *)il_ImageList_LoadImageA },
	E(ImageList_LoadImageA),
	E(ImageList_LoadImageW),
	E(ImageList_Merge),
	E(ImageList_Read),
	E(ImageList_Remove),
	E(ImageList_Replace),
	E(ImageList_ReplaceIcon),
	E(ImageList_SetBkColor),
	{ "ImageList_SetColorTable", 390, (void *)il_ImageList_SetColorTable },
	E(ImageList_SetDragCursorImage),
	E(ImageList_SetFilter),
	E(ImageList_SetFlags),
	E(ImageList_SetIconSize),
	E(ImageList_SetImageCount),
	E(ImageList_SetOverlayImage),
	E(ImageList_Write),
	BUILTIN_END
};
