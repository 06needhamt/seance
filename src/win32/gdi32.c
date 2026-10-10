/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gdi32.dll: the object table, DCs, palettes, brushes, pens and fonts.
 * Bitmaps, DIBs and blits are in gdi_bitmap.c. See gdi.h for the model.
 *
 * Text is measured and drawn by gdi_text.c, with one bitmap font scaled
 * to each LOGFONT. Mapping modes and ROP2 are stored and reported back but
 * not applied (every DC works in MM_TEXT pixels).
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "win32/codepage.h"
#include "win32/gdi.h"
#include "win32/internal.h"

#define MAX_OBJECTS 16384

#define MM_TEXT        1
#define R2_COPYPEN     13
#define SYSPAL_STATIC  1

static struct gdi_obj *objs[MAX_OBJECTS];
static BYTE gens[MAX_OBJECTS];
static size_t next_free = 1;
static pthread_mutex_t lock;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static HGDIOBJ stock[DC_PEN + 1];
static HBITMAP default_bitmap;
static HBRUSH sys_brushes[31];

const PALETTEENTRY gdi_default_palette[20] = {
	{ 0x00, 0x00, 0x00, 0 }, { 0x80, 0x00, 0x00, 0 }, { 0x00, 0x80, 0x00, 0 },
	{ 0x80, 0x80, 0x00, 0 }, { 0x00, 0x00, 0x80, 0 }, { 0x80, 0x00, 0x80, 0 },
	{ 0x00, 0x80, 0x80, 0 }, { 0xc0, 0xc0, 0xc0, 0 }, { 0xc0, 0xdc, 0xc0, 0 },
	{ 0xa6, 0xca, 0xf0, 0 }, { 0xff, 0xfb, 0xf0, 0 }, { 0xa0, 0xa0, 0xa4, 0 },
	{ 0x80, 0x80, 0x80, 0 }, { 0xff, 0x00, 0x00, 0 }, { 0x00, 0xff, 0x00, 0 },
	{ 0xff, 0xff, 0x00, 0 }, { 0x00, 0x00, 0xff, 0 }, { 0xff, 0x00, 0xff, 0 },
	{ 0x00, 0xff, 0xff, 0 }, { 0xff, 0xff, 0xff, 0 },
};

/* The first ten static colours, a 6x6x6 cube, greys, the last ten. */
void gdi_halftone_palette(PALETTEENTRY out[256])
{
	int n = 0;

	for (int i = 0; i < 10; i++)
		out[n++] = gdi_default_palette[i];
	for (int r = 0; r < 6; r++)
		for (int g = 0; g < 6; g++)
			for (int b = 0; b < 6; b++)
				out[n++] = (PALETTEENTRY){ r * 51, g * 51, b * 51, 0 };
	for (int i = 1; n < 246; i++) {
		BYTE v = i * 255 / 21;
		out[n++] = (PALETTEENTRY){ v, v, v, 0 };
	}
	for (int i = 10; i < 20; i++)
		out[n++] = gdi_default_palette[i];
}

/* The object table. */

static BYTE type_code(int type)
{
	switch (type) {
	case OBJ_DC:
	case OBJ_MEMDC:  return 0x01;
	case OBJ_REGION: return 0x04;
	case OBJ_BITMAP: return 0x05;
	case OBJ_PAL:    return 0x08;
	case OBJ_FONT:   return 0x0a;
	case OBJ_BRUSH:  return 0x10;
	case OBJ_PEN:    return 0x30;
	default:         return 0x7f;
	}
}

void gdi_free(struct gdi_obj *o)
{
	size_t i = (ULONG_PTR)o->handle & 0xffff;

	switch (o->type) {
	case OBJ_BITMAP:
		gdi_bitmap_free(&o->u.bmp);
		break;
	case OBJ_PAL:
		free(o->u.pal.entries);
		break;
	case OBJ_ENHMETAFILE:
		free(o->u.emf.bits);
		break;
	case OBJ_DC:
	case OBJ_MEMDC:
		free(o->u.dc.saved);
		break;
	}
	objs[i] = NULL;
	gens[i]++;
	if (i < next_free)
		next_free = i;
	free(o);
}

struct gdi_obj *gdi_new(int type)
{
	struct gdi_obj *o = calloc(1, sizeof(*o));
	size_t i = next_free;

	while (i < MAX_OBJECTS && objs[i])
		i++;
	if (!o || i >= MAX_OBJECTS) {
		free(o);
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	next_free = i + 1;
	objs[i] = o;
	o->type = type;
	o->handle = (HGDIOBJ)(ULONG_PTR)((DWORD)gens[i] << 24 |
					 (DWORD)type_code(type) << 16 | i);
	return o;
}

struct gdi_obj *gdi_get(HGDIOBJ h, int type)
{
	size_t i = (ULONG_PTR)h & 0xffff;
	struct gdi_obj *o = i < MAX_OBJECTS ? objs[i] : NULL;

	if (!o || o->handle != h ||
	    (type && o->type != type &&
	     !(type == OBJ_DC && o->type == OBJ_MEMDC))) {
		win_set_error(ERROR_INVALID_HANDLE);
		return NULL;
	}
	return o;
}

static HGDIOBJ new_brush(UINT style, COLORREF color, int is_stock)
{
	struct gdi_obj *o = gdi_new(OBJ_BRUSH);

	if (!o)
		return NULL;
	o->u.brush.lbStyle = style;
	o->u.brush.lbColor = color;
	o->stock = is_stock;
	return o->handle;
}

WINFN(HBRUSH) gdi_CreatePatternBrush(HBITMAP bitmap)
{
	gdi_lock();
	if (!gdi_get((HGDIOBJ)bitmap, OBJ_BITMAP)) { gdi_unlock(); return NULL; }
	HBRUSH brush = (HBRUSH)new_brush(BS_PATTERN, (COLORREF)(ULONG_PTR)bitmap, 0);
	gdi_unlock();
	return brush;
}

WINFN(HGDIOBJ) gdi_SetEnhMetaFileBits(UINT size, const BYTE *bits)
{
	if (!size || !bits) return NULL;
	gdi_lock();
	struct gdi_obj *o = gdi_new(OBJ_ENHMETAFILE);
	if (o) {
		o->u.emf.bits = malloc(size);
		if (!o->u.emf.bits) { gdi_free(o); o = NULL; }
		else { memcpy(o->u.emf.bits, bits, size); o->u.emf.size = size; }
	}
	HGDIOBJ h = o ? o->handle : NULL;
	gdi_unlock();
	return h;
}

WINFN(BOOL) gdi_DeleteEnhMetaFile(HGDIOBJ emf)
{
	gdi_lock();
	struct gdi_obj *o = gdi_get(emf, OBJ_ENHMETAFILE);
	if (o)
		gdi_free(o);
	gdi_unlock();
	return o != NULL;
}

WINFN(BOOL) gdi_PlayEnhMetaFile(HDC dc, HGDIOBJ emf, const RECT *bounds)
{
	(void)bounds;
	gdi_lock();
	BOOL ok = gdi_get((HGDIOBJ)dc, OBJ_DC) && gdi_get(emf, OBJ_ENHMETAFILE);
	gdi_unlock(); return ok;
}

static HGDIOBJ new_pen(UINT style, int width, COLORREF color, int is_stock)
{
	struct gdi_obj *o = gdi_new(OBJ_PEN);

	if (!o)
		return NULL;
	o->u.pen.lopnStyle = style;
	o->u.pen.lopnWidth.x = width;
	o->u.pen.lopnColor = color;
	o->stock = is_stock;
	return o->handle;
}

static HGDIOBJ new_font(const LOGFONTW *lf, int is_stock)
{
	struct gdi_obj *o = gdi_new(OBJ_FONT);

	if (!o)
		return NULL;
	o->u.font = *lf;
	o->stock = is_stock;
	return o->handle;
}

static HGDIOBJ stock_font(int height, int pitch, const char *face)
{
	LOGFONTW lf;

	memset(&lf, 0, sizeof(lf));
	lf.lfHeight = height;
	lf.lfWeight = FW_NORMAL;
	lf.lfPitchAndFamily = pitch;
	for (int i = 0; face[i] && i < LF_FACESIZE - 1; i++)
		lf.lfFaceName[i] = (BYTE)face[i];
	return new_font(&lf, 1);
}

static void init(void)
{
	pthread_mutexattr_t attr;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&lock, &attr);
	pthread_mutexattr_destroy(&attr);

	stock[WHITE_BRUSH] = new_brush(BS_SOLID, RGB(255, 255, 255), 1);
	stock[LTGRAY_BRUSH] = new_brush(BS_SOLID, RGB(192, 192, 192), 1);
	stock[GRAY_BRUSH] = new_brush(BS_SOLID, RGB(128, 128, 128), 1);
	stock[DKGRAY_BRUSH] = new_brush(BS_SOLID, RGB(64, 64, 64), 1);
	stock[BLACK_BRUSH] = new_brush(BS_SOLID, RGB(0, 0, 0), 1);
	stock[NULL_BRUSH] = new_brush(BS_NULL, 0, 1);
	stock[WHITE_PEN] = new_pen(PS_SOLID, 0, RGB(255, 255, 255), 1);
	stock[BLACK_PEN] = new_pen(PS_SOLID, 0, RGB(0, 0, 0), 1);
	stock[NULL_PEN] = new_pen(PS_NULL, 0, 0, 1);
	stock[OEM_FIXED_FONT] = stock_font(12, 1, "Terminal");
	stock[ANSI_FIXED_FONT] = stock_font(12, 1, "Courier");
	stock[ANSI_VAR_FONT] = stock_font(12, 2, "MS Sans Serif");
	stock[SYSTEM_FONT] = stock_font(16, 2, "System");
	stock[DEVICE_DEFAULT_FONT] = stock_font(16, 2, "System");
	stock[SYSTEM_FIXED_FONT] = stock_font(16, 1, "Fixedsys");
	stock[DEFAULT_GUI_FONT] = stock_font(-11, 2, "MS Shell Dlg");
	stock[DC_BRUSH] = new_brush(BS_SOLID, RGB(255, 255, 255), 1);
	stock[DC_PEN] = new_pen(PS_SOLID, 0, RGB(0, 0, 0), 1);

	struct gdi_obj *pal = gdi_new(OBJ_PAL);
	if (pal && (pal->u.pal.entries = malloc(sizeof(gdi_default_palette)))) {
		memcpy(pal->u.pal.entries, gdi_default_palette,
		       sizeof(gdi_default_palette));
		pal->u.pal.n = 20;
		pal->stock = 1;
		stock[DEFAULT_PALETTE] = pal->handle;
	}
	struct gdi_obj *bmp = gdi_new(OBJ_BITMAP);
	if (bmp) {
		gdi_bitmap_init_default(&bmp->u.bmp);
		bmp->stock = 1;
		default_bitmap = bmp->handle;
	}
}

void gdi_lock(void)
{
	pthread_once(&once, init);
	pthread_mutex_lock(&lock);
}

void gdi_unlock(void)
{
	pthread_mutex_unlock(&lock);
}

HBITMAP gdi_default_bitmap(void)
{
	pthread_once(&once, init);
	return default_bitmap;
}

HBRUSH gdi_sys_color_brush(int index, COLORREF color)
{
	HBRUSH h = NULL;

	if (index < 0 || index >= (int)(sizeof(sys_brushes) / sizeof(sys_brushes[0])))
		return NULL;
	gdi_lock();
	if (!sys_brushes[index])
		sys_brushes[index] = new_brush(BS_SOLID, color, 1);
	h = sys_brushes[index];
	gdi_unlock();
	return h;
}

COLORREF gdi_palette_color(const struct gdi_dc *dc, WORD i)
{
	struct gdi_obj *p = gdi_get(dc ? dc->s.palette : stock[DEFAULT_PALETTE],
				    OBJ_PAL);

	if (!p || i >= p->u.pal.n)
		return 0;
	const PALETTEENTRY *e = &p->u.pal.entries[i];
	return RGB(e->peRed, e->peGreen, e->peBlue);
}

/* Stock objects and generic object functions. */

WINFN(HGDIOBJ) gdi_GetStockObject(int i)
{
	HGDIOBJ h = NULL;

	gdi_lock();
	if (i >= 0 && i <= DC_PEN)
		h = stock[i];
	gdi_unlock();
	return h;
}

WINFN(DWORD) gdi_GetObjectType(HGDIOBJ h)
{
	gdi_lock();
	struct gdi_obj *o = gdi_get(h, 0);
	DWORD t = o ? o->type : 0;
	gdi_unlock();
	return t;
}

/* Whether any DC has o selected. */
static int is_selected(const struct gdi_obj *o)
{
	if (o->type == OBJ_BITMAP)
		return o->u.bmp.selected != NULL;
	for (size_t i = 0; i < MAX_OBJECTS; i++) {
		const struct gdi_obj *d = objs[i];
		if (!d || (d->type != OBJ_DC && d->type != OBJ_MEMDC))
			continue;
		const struct gdi_dc_state *s = &d->u.dc.s;
		if (s->brush == o->handle || s->pen == o->handle ||
		    s->font == o->handle || s->palette == o->handle)
			return 1;
	}
	return 0;
}

WINFN(BOOL) gdi_DeleteObject(HGDIOBJ h)
{
	BOOL ok = FALSE;

	gdi_lock();
	struct gdi_obj *o = gdi_get(h, 0);
	if (o && o->stock) {
		ok = TRUE;
	} else if (o && o->type != OBJ_DC && o->type != OBJ_MEMDC &&
		   !is_selected(o)) {
		gdi_free(o);
		ok = TRUE;
	}
	gdi_unlock();
	return ok;
}

WINFN(HGDIOBJ) gdi_SelectObject(HDC hdc, HGDIOBJ h)
{
	HGDIOBJ prev = NULL;

	gdi_lock();
	struct gdi_obj *d = gdi_get(hdc, OBJ_DC), *o = gdi_get(h, 0);
	if (d && o) {
		struct gdi_dc_state *s = &d->u.dc.s;
		switch (o->type) {
		case OBJ_BITMAP: {
			struct gdi_obj *old = gdi_get(s->bitmap, OBJ_BITMAP);
			if (!d->u.dc.memory ||
			    (!o->stock && o->u.bmp.selected &&
			     o->u.bmp.selected != hdc))
				break;
			prev = s->bitmap;
			if (old && !old->stock)
				old->u.bmp.selected = NULL;
			if (!o->stock)
				o->u.bmp.selected = hdc;
			s->bitmap = h;
			break;
		}
		case OBJ_BRUSH:
			prev = s->brush;
			s->brush = h;
			break;
		case OBJ_PEN:
			prev = s->pen;
			s->pen = h;
			break;
		case OBJ_FONT:
			prev = s->font;
			s->font = h;
			break;
		default:
			win_set_error(ERROR_INVALID_PARAMETER);
			break;
		}
	}
	gdi_unlock();
	return prev;
}

WINFN(HGDIOBJ) gdi_GetCurrentObject(HDC hdc, UINT type)
{
	HGDIOBJ h = NULL;

	gdi_lock();
	struct gdi_obj *d = gdi_get(hdc, OBJ_DC);
	if (d) {
		switch (type) {
		case OBJ_BITMAP: h = d->u.dc.s.bitmap; break;
		case OBJ_BRUSH:  h = d->u.dc.s.brush; break;
		case OBJ_PEN:    h = d->u.dc.s.pen; break;
		case OBJ_FONT:   h = d->u.dc.s.font; break;
		case OBJ_PAL:    h = d->u.dc.s.palette; break;
		}
	}
	gdi_unlock();
	return h;
}

static void fill_bitmap(const struct gdi_bitmap *b, BITMAP *bm)
{
	bm->bmType = 0;
	bm->bmWidth = b->width;
	bm->bmHeight = b->height;
	bm->bmWidthBytes = b->dib ? b->stride : ((b->width * b->bpp + 15) / 16) * 2;
	bm->bmPlanes = 1;
	bm->bmBitsPixel = b->bpp;
	bm->bmBits = b->dib ? b->bits : NULL;
}

static int get_object(HGDIOBJ h, int size, void *buf, int wide)
{
	int r = 0;

	gdi_lock();
	struct gdi_obj *o = gdi_get(h, 0);
	if (!o)
		goto out;
	switch (o->type) {
	case OBJ_BITMAP: {
		const struct gdi_bitmap *b = &o->u.bmp;
		if (!buf) {
			r = b->dib ? sizeof(DIBSECTION) : sizeof(BITMAP);
		} else if (b->dib && size >= (int)sizeof(DIBSECTION)) {
			DIBSECTION *ds = buf;
			memset(ds, 0, sizeof(*ds));
			fill_bitmap(b, &ds->dsBm);
			ds->dsBmih.biSize = sizeof(BITMAPINFOHEADER);
			ds->dsBmih.biWidth = b->width;
			ds->dsBmih.biHeight = b->top_down ? -b->height : b->height;
			ds->dsBmih.biPlanes = 1;
			ds->dsBmih.biBitCount = b->bpp;
			ds->dsBmih.biCompression = b->compression;
			ds->dsBmih.biSizeImage = b->stride * b->height;
			ds->dsBmih.biClrUsed = b->bpp <= 8 ? b->ncolors : 0;
			if (b->compression == BI_BITFIELDS)
				memcpy(ds->dsBitfields, b->masks, sizeof(b->masks));
			ds->dshSection = b->section;
			ds->dsOffset = b->offset;
			r = sizeof(DIBSECTION);
		} else if (size >= (int)sizeof(BITMAP)) {
			fill_bitmap(b, buf);
			r = sizeof(BITMAP);
		}
		break;
	}
	case OBJ_PAL:
		if (!buf) {
			r = sizeof(WORD);
		} else if (size >= (int)sizeof(WORD)) {
			*(WORD *)buf = o->u.pal.n;
			r = sizeof(WORD);
		}
		break;
	case OBJ_BRUSH:
		r = size < (int)sizeof(LOGBRUSH) ? size : (int)sizeof(LOGBRUSH);
		if (!buf)
			r = sizeof(LOGBRUSH);
		else
			memcpy(buf, &o->u.brush, r);
		break;
	case OBJ_PEN:
		r = size < (int)sizeof(LOGPEN) ? size : (int)sizeof(LOGPEN);
		if (!buf)
			r = sizeof(LOGPEN);
		else
			memcpy(buf, &o->u.pen, r);
		break;
	case OBJ_FONT:
		if (wide) {
			r = size < (int)sizeof(LOGFONTW) ? size : (int)sizeof(LOGFONTW);
			if (!buf)
				r = sizeof(LOGFONTW);
			else
				memcpy(buf, &o->u.font, r);
		} else {
			LOGFONTA a;
			memcpy(&a, &o->u.font, offsetof(LOGFONTA, lfFaceName));
			cp_from_utf16(CP_ACP, o->u.font.lfFaceName, -1, a.lfFaceName,
				      LF_FACESIZE, '?', NULL);
			a.lfFaceName[LF_FACESIZE - 1] = '\0';
			r = size < (int)sizeof(a) ? size : (int)sizeof(a);
			if (!buf)
				r = sizeof(a);
			else
				memcpy(buf, &a, r);
		}
		break;
	}
out:
	gdi_unlock();
	return r;
}

WINFN(int) gdi_GetObjectA(HGDIOBJ h, int size, LPVOID buf)
{
	return get_object(h, size, buf, 0);
}

WINFN(int) gdi_GetObjectW(HGDIOBJ h, int size, LPVOID buf)
{
	return get_object(h, size, buf, 1);
}

/* DCs. */

static HDC new_dc(int memory, HWND hwnd)
{
	struct gdi_obj *o = gdi_new(memory ? OBJ_MEMDC : OBJ_DC);

	if (!o)
		return NULL;
	struct gdi_dc *dc = &o->u.dc;
	dc->memory = memory;
	dc->hwnd = hwnd;
	dc->s.bitmap = memory ? default_bitmap : NULL;
	dc->s.brush = stock[WHITE_BRUSH];
	dc->s.pen = stock[BLACK_PEN];
	dc->s.font = stock[SYSTEM_FONT];
	dc->s.palette = stock[DEFAULT_PALETTE];
	dc->s.text = RGB(0, 0, 0);
	dc->s.bk = RGB(255, 255, 255);
	dc->s.bkmode = OPAQUE;
	dc->s.stretch_mode = BLACKONWHITE;
	dc->s.map_mode = MM_TEXT;
	dc->s.rop2 = R2_COPYPEN;
	return o->handle;
}

HDC gdi_display_dc(HWND hwnd)
{
	gdi_lock();
	HDC h = new_dc(0, hwnd);
	gdi_unlock();
	return h;
}

static void release_selection(struct gdi_obj *d)
{
	struct gdi_obj *b = gdi_get(d->u.dc.s.bitmap, OBJ_BITMAP);

	if (b && !b->stock && b->u.bmp.selected == d->handle)
		b->u.bmp.selected = NULL;
}

int gdi_release_dc(HWND hwnd, HDC hdc)
{
	int ok = 0;

	gdi_lock();
	struct gdi_obj *d = gdi_get(hdc, OBJ_DC);
	if (d && !d->u.dc.memory) {
		gdi_free(d);
		ok = 1;
	}
	gdi_unlock();
	return ok;
}

WINFN(HDC) gdi_CreateCompatibleDC(HDC hdc)
{
	gdi_lock();
	HDC h = (!hdc || gdi_get(hdc, OBJ_DC)) ? new_dc(1, NULL) : NULL;
	gdi_unlock();
	return h;
}

static int is_display(const char *driver, const char *device)
{
	if (driver && !strcasecmp(driver, "DISPLAY"))
		return 1;
	return !driver && device &&
	       (!strcasecmp(device, "DISPLAY") ||
		!strncasecmp(device, "\\\\.\\DISPLAY", 11));
}

static HDC create_dc(const char *driver, const char *device)
{
	if (!is_display(driver, device)) {
		TRACE("CreateDC(%s, %s): only the display is supported",
		      driver ? driver : "NULL", device ? device : "NULL");
		win_set_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	return gdi_display_dc(NULL);
}

WINFN(HDC) gdi_CreateDCA(LPCSTR driver, LPCSTR device, LPCSTR output,
			 const void *devmode)
{
	return create_dc(driver, device);
}

WINFN(HDC) gdi_CreateDCW(LPCWSTR driver, LPCWSTR device, LPCWSTR output,
			 const void *devmode)
{
	char *d = utf16_to_utf8_dup(driver), *v = utf16_to_utf8_dup(device);
	HDC h = create_dc(d, v);
	free(d);
	free(v);
	return h;
}

WINFN(BOOL) gdi_DeleteDC(HDC hdc)
{
	BOOL ok = FALSE;

	gdi_lock();
	struct gdi_obj *d = gdi_get(hdc, OBJ_DC);
	if (d) {
		release_selection(d);
		gdi_free(d);
		ok = TRUE;
	}
	gdi_unlock();
	return ok;
}

WINFN(int) gdi_SaveDC(HDC hdc)
{
	int r = 0;

	gdi_lock();
	struct gdi_obj *d = gdi_get(hdc, OBJ_DC);
	if (d) {
		struct gdi_dc *dc = &d->u.dc;
		struct gdi_dc_state *s = realloc(dc->saved,
						 (dc->nsaved + 1) * sizeof(*s));
		if (s) {
			dc->saved = s;
			s[dc->nsaved++] = dc->s;
			r = dc->nsaved;
		}
	}
	gdi_unlock();
	return r;
}

WINFN(BOOL) gdi_RestoreDC(HDC hdc, int level)
{
	BOOL ok = FALSE;

	gdi_lock();
	struct gdi_obj *d = gdi_get(hdc, OBJ_DC);
	if (d) {
		struct gdi_dc *dc = &d->u.dc;
		if (level < 0)
			level = dc->nsaved + 1 + level;
		if (level >= 1 && level <= dc->nsaved) {
			struct gdi_dc_state s = dc->saved[level - 1];
			dc->nsaved = level - 1;
			/*
			 * The saved bitmap comes back if it can still be
			 * selected; other objects deleted since fall back to
			 * the defaults.
			 */
			if (s.bitmap != dc->s.bitmap) {
				struct gdi_obj *b = gdi_get(s.bitmap, OBJ_BITMAP);
				if (b && (b->stock || !b->u.bmp.selected)) {
					release_selection(d);
					if (!b->stock)
						b->u.bmp.selected = hdc;
				} else {
					s.bitmap = dc->s.bitmap;
				}
			}
			if (!gdi_get(s.brush, OBJ_BRUSH))
				s.brush = stock[WHITE_BRUSH];
			if (!gdi_get(s.pen, OBJ_PEN))
				s.pen = stock[BLACK_PEN];
			if (!gdi_get(s.font, OBJ_FONT))
				s.font = stock[SYSTEM_FONT];
			if (!gdi_get(s.palette, OBJ_PAL))
				s.palette = stock[DEFAULT_PALETTE];
			dc->s = s;
			ok = TRUE;
		}
	}
	gdi_unlock();
	return ok;
}

WINFN(int) gdi_GetDeviceCaps(HDC hdc, int index)
{
	gdi_lock();
	struct gdi_obj *d = gdi_get(hdc, OBJ_DC);
	gdi_unlock();
	if (!d)
		return 0;
	switch (index) {
	case DRIVERVERSION:  return 0x4000;
	case TECHNOLOGY:     return DT_RASDISPLAY;
	case HORZSIZE:       return GDI_SCREEN_WIDTH * 254 / 960;
	case VERTSIZE:       return GDI_SCREEN_HEIGHT * 254 / 960;
	case HORZRES:
	case DESKTOPHORZRES: return GDI_SCREEN_WIDTH;
	case VERTRES:
	case DESKTOPVERTRES: return GDI_SCREEN_HEIGHT;
	case BITSPIXEL:      return GDI_SCREEN_BPP;
	case PLANES:         return 1;
	case NUMBRUSHES:
	case NUMPENS:
	case NUMCOLORS:      return -1;
	case ASPECTX:
	case ASPECTY:        return 36;
	case ASPECTXY:       return 51;
	case LOGPIXELSX:
	case LOGPIXELSY:     return 96;
	case NUMRESERVED:    return 20;
	case COLORRES:       return 24;
	case RASTERCAPS:
		return RC_BITBLT | RC_BITMAP64 | RC_DI_BITMAP | RC_DIBTODEV |
		       RC_STRETCHBLT | RC_STRETCHDIB;
	case VREFRESH:       return 60;
	default:             return 0;
	}
}

/* Reads or writes one field of a DC's state; returns the old value. */
enum dc_field { F_TEXT, F_BK, F_BKMODE, F_STRETCH, F_MAPMODE, F_ROP2, F_ALIGN };

static DWORD dc_field(HDC hdc, enum dc_field f, int set, DWORD v, DWORD fail)
{
	DWORD old = fail;

	gdi_lock();
	struct gdi_obj *d = gdi_get(hdc, OBJ_DC);
	if (d) {
		struct gdi_dc_state *s = &d->u.dc.s;
		DWORD *p = NULL;
		int *ip = NULL;
		switch (f) {
		case F_TEXT:    p = &s->text; break;
		case F_BK:      p = &s->bk; break;
		case F_ALIGN:   p = &s->text_align; break;
		case F_BKMODE:  ip = &s->bkmode; break;
		case F_STRETCH: ip = &s->stretch_mode; break;
		case F_MAPMODE: ip = &s->map_mode; break;
		case F_ROP2:    ip = &s->rop2; break;
		}
		old = p ? *p : (DWORD)*ip;
		if (set && p)
			*p = v;
		else if (set)
			*ip = v;
	}
	gdi_unlock();
	return old;
}

WINFN(COLORREF) gdi_SetTextColor(HDC h, COLORREF c) { return dc_field(h, F_TEXT, 1, c, CLR_INVALID); }
WINFN(COLORREF) gdi_GetTextColor(HDC h) { return dc_field(h, F_TEXT, 0, 0, CLR_INVALID); }
WINFN(COLORREF) gdi_SetBkColor(HDC h, COLORREF c) { return dc_field(h, F_BK, 1, c, CLR_INVALID); }
WINFN(COLORREF) gdi_GetBkColor(HDC h) { return dc_field(h, F_BK, 0, 0, CLR_INVALID); }
WINFN(int) gdi_SetBkMode(HDC h, int m) { return dc_field(h, F_BKMODE, 1, m, 0); }
WINFN(int) gdi_GetBkMode(HDC h) { return dc_field(h, F_BKMODE, 0, 0, 0); }
WINFN(int) gdi_SetStretchBltMode(HDC h, int m) { return dc_field(h, F_STRETCH, 1, m, 0); }
WINFN(int) gdi_GetStretchBltMode(HDC h) { return dc_field(h, F_STRETCH, 0, 0, 0); }
WINFN(int) gdi_SetMapMode(HDC h, int m) { return dc_field(h, F_MAPMODE, 1, m, 0); }
WINFN(int) gdi_GetMapMode(HDC h) { return dc_field(h, F_MAPMODE, 0, 0, 0); }
WINFN(int) gdi_SetROP2(HDC h, int m) { return dc_field(h, F_ROP2, 1, m, 0); }
WINFN(int) gdi_GetROP2(HDC h) { return dc_field(h, F_ROP2, 0, 0, 0); }
WINFN(UINT) gdi_SetTextAlign(HDC h, UINT a) { return dc_field(h, F_ALIGN, 1, a, GDI_ERROR); }
WINFN(UINT) gdi_GetTextAlign(HDC h) { return dc_field(h, F_ALIGN, 0, 0, GDI_ERROR); }

WINFN(BOOL) gdi_MoveToEx(HDC hdc, int x, int y, LPPOINT old)
{
	BOOL ok = FALSE;

	gdi_lock();
	struct gdi_obj *d = gdi_get(hdc, OBJ_DC);
	if (d) {
		if (old)
			*old = d->u.dc.s.pos;
		d->u.dc.s.pos.x = x;
		d->u.dc.s.pos.y = y;
		ok = TRUE;
	}
	gdi_unlock();
	return ok;
}

WINFN(BOOL) gdi_GdiFlush(void)
{
	return TRUE;
}

/* Palettes. */

static HPALETTE new_palette(const PALETTEENTRY *e, WORD n)
{
	struct gdi_obj *o = gdi_new(OBJ_PAL);

	if (!o)
		return NULL;
	if (!(o->u.pal.entries = malloc((n ? n : 1) * sizeof(*e)))) {
		gdi_free(o);
		return NULL;
	}
	memcpy(o->u.pal.entries, e, n * sizeof(*e));
	o->u.pal.n = n;
	return o->handle;
}

WINFN(HPALETTE) gdi_CreatePalette(const LOGPALETTE *lp)
{
	if (!lp || lp->palVersion != 0x300) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	gdi_lock();
	HPALETTE h = new_palette(lp->palPalEntry, lp->palNumEntries);
	gdi_unlock();
	return h;
}

WINFN(HPALETTE) gdi_CreateHalftonePalette(HDC hdc)
{
	PALETTEENTRY e[256];

	gdi_halftone_palette(e);
	gdi_lock();
	HPALETTE h = new_palette(e, 256);
	gdi_unlock();
	return h;
}

static UINT palette_entries(HPALETTE h, UINT start, UINT n,
			    PALETTEENTRY *e, int set)
{
	UINT r = 0;

	gdi_lock();
	struct gdi_obj *o = gdi_get(h, OBJ_PAL);
	if (o && !set && !e) {
		r = o->u.pal.n;
	} else if (o && e && start < o->u.pal.n && !(set && o->stock)) {
		r = o->u.pal.n - start < n ? o->u.pal.n - start : n;
		if (set)
			memcpy(&o->u.pal.entries[start], e, r * sizeof(*e));
		else
			memcpy(e, &o->u.pal.entries[start], r * sizeof(*e));
	}
	gdi_unlock();
	return r;
}

WINFN(UINT) gdi_GetPaletteEntries(HPALETTE h, UINT start, UINT n,
				  LPPALETTEENTRY e)
{
	return palette_entries(h, start, n, e, 0);
}

WINFN(UINT) gdi_SetPaletteEntries(HPALETTE h, UINT start, UINT n,
				  const PALETTEENTRY *e)
{
	return palette_entries(h, start, n, (PALETTEENTRY *)e, 1);
}

WINFN(UINT) gdi_GetNearestPaletteIndex(HPALETTE h, COLORREF c)
{
	UINT best = CLR_INVALID;
	long best_d = -1;

	gdi_lock();
	struct gdi_obj *o = gdi_get(h, OBJ_PAL);
	for (UINT i = 0; o && i < o->u.pal.n; i++) {
		const PALETTEENTRY *e = &o->u.pal.entries[i];
		long dr = e->peRed - GetRValue(c), dg = e->peGreen - GetGValue(c);
		long db = e->peBlue - GetBValue(c), d = dr * dr + dg * dg + db * db;
		if (best_d < 0 || d < best_d) {
			best_d = d;
			best = i;
		}
	}
	gdi_unlock();
	return best;
}

WINFN(HPALETTE) gdi_SelectPalette(HDC hdc, HPALETTE h, BOOL background)
{
	HPALETTE prev = NULL;

	gdi_lock();
	struct gdi_obj *d = gdi_get(hdc, OBJ_DC);
	if (d && gdi_get(h, OBJ_PAL)) {
		prev = d->u.dc.s.palette;
		d->u.dc.s.palette = h;
	}
	gdi_unlock();
	return prev;
}

/* A true-colour device has no system palette to realise into. */
WINFN(UINT) gdi_RealizePalette(HDC hdc)
{
	return 0;
}

WINFN(UINT) gdi_GetSystemPaletteEntries(HDC hdc, UINT start, UINT n,
					LPPALETTEENTRY e)
{
	return 0;
}

WINFN(UINT) gdi_GetSystemPaletteUse(HDC hdc)
{
	return SYSPAL_STATIC;
}

WINFN(COLORREF) gdi_GetNearestColor(HDC hdc, COLORREF c)
{
	return c & 0xffffff;
}

/* Brushes, pens and fonts. */

WINFN(HBRUSH) gdi_CreateSolidBrush(COLORREF c)
{
	gdi_lock();
	HBRUSH h = new_brush(BS_SOLID, c, 0);
	gdi_unlock();
	return h;
}

WINFN(HBRUSH) gdi_CreateBrushIndirect(const LOGBRUSH *lb)
{
	if (!lb)
		return NULL;
	gdi_lock();
	HBRUSH h = new_brush(lb->lbStyle, lb->lbColor, 0);
	gdi_unlock();
	return h;
}

WINFN(HPEN) gdi_CreatePen(int style, int width, COLORREF c)
{
	gdi_lock();
	HPEN h = new_pen(style, width, c, 0);
	gdi_unlock();
	return h;
}

WINFN(HPEN) gdi_CreatePenIndirect(const LOGPEN *lp)
{
	if (!lp)
		return NULL;
	return gdi_CreatePen(lp->lopnStyle, lp->lopnWidth.x, lp->lopnColor);
}

static HFONT create_font(const LOGFONTW *lf)
{
	gdi_lock();
	HFONT h = new_font(lf, 0);
	gdi_unlock();
	return h;
}

WINFN(HFONT) gdi_CreateFontIndirectW(const LOGFONTW *lf)
{
	return lf ? create_font(lf) : NULL;
}

WINFN(HFONT) gdi_CreateFontIndirectA(const LOGFONTA *lf)
{
	LOGFONTW w;

	if (!lf)
		return NULL;
	memcpy(&w, lf, offsetof(LOGFONTW, lfFaceName));
	memset(w.lfFaceName, 0, sizeof(w.lfFaceName));
	cp_to_utf16(CP_ACP, lf->lfFaceName, strnlen(lf->lfFaceName, LF_FACESIZE - 1),
		    w.lfFaceName, LF_FACESIZE - 1, 0);
	return create_font(&w);
}

static void make_logfont(LOGFONTW *lf, int h, int w, int esc, int orient,
			 int weight, DWORD italic, DWORD underline,
			 DWORD strike, DWORD charset, DWORD out, DWORD clip,
			 DWORD quality, DWORD pitch)
{
	memset(lf, 0, sizeof(*lf));
	lf->lfHeight = h;
	lf->lfWidth = w;
	lf->lfEscapement = esc;
	lf->lfOrientation = orient;
	lf->lfWeight = weight;
	lf->lfItalic = italic;
	lf->lfUnderline = underline;
	lf->lfStrikeOut = strike;
	lf->lfCharSet = charset;
	lf->lfOutPrecision = out;
	lf->lfClipPrecision = clip;
	lf->lfQuality = quality;
	lf->lfPitchAndFamily = pitch;
}

WINFN(HFONT) gdi_CreateFontA(int h, int w, int esc, int orient, int weight,
			     DWORD italic, DWORD underline, DWORD strike,
			     DWORD charset, DWORD out, DWORD clip,
			     DWORD quality, DWORD pitch, LPCSTR face)
{
	LOGFONTW lf;

	make_logfont(&lf, h, w, esc, orient, weight, italic, underline, strike,
		     charset, out, clip, quality, pitch);
	if (face)
		cp_to_utf16(CP_ACP, face, strnlen(face, LF_FACESIZE - 1),
			    lf.lfFaceName, LF_FACESIZE - 1, 0);
	return create_font(&lf);
}

WINFN(HFONT) gdi_CreateFontW(int h, int w, int esc, int orient, int weight,
			     DWORD italic, DWORD underline, DWORD strike,
			     DWORD charset, DWORD out, DWORD clip,
			     DWORD quality, DWORD pitch, LPCWSTR face)
{
	LOGFONTW lf;

	make_logfont(&lf, h, w, esc, orient, weight, italic, underline, strike,
		     charset, out, clip, quality, pitch);
	for (int i = 0; face && face[i] && i < LF_FACESIZE - 1; i++)
		lf.lfFaceName[i] = face[i];
	return create_font(&lf);
}

/* Text: gdi_text.c measures and draws it. */

WINFN(BOOL) gdi_GetTextMetricsW(HDC hdc, LPTEXTMETRICW tm)
{
	return tm && gdi_text_metrics(hdc, tm);
}

WINFN(BOOL) gdi_GetTextMetricsA(HDC hdc, LPTEXTMETRICA tm)
{
	TEXTMETRICW w;

	if (!tm || !gdi_text_metrics(hdc, &w))
		return FALSE;
	memcpy(tm, &w, offsetof(TEXTMETRICA, tmFirstChar));
	tm->tmFirstChar = w.tmFirstChar;
	tm->tmLastChar = w.tmLastChar;
	tm->tmDefaultChar = w.tmDefaultChar;
	tm->tmBreakChar = w.tmBreakChar;
	tm->tmItalic = w.tmItalic;
	tm->tmUnderlined = w.tmUnderlined;
	tm->tmStruckOut = w.tmStruckOut;
	tm->tmPitchAndFamily = w.tmPitchAndFamily;
	tm->tmCharSet = w.tmCharSet;
	return TRUE;
}

static BOOL text_extent(HDC hdc, int n, LPSIZE size)
{
	int w, h;

	if (!size || !gdi_text_width(hdc, n, &w, &h))
		return FALSE;
	size->cx = w;
	size->cy = h;
	return TRUE;
}

/* An ANSI string as UTF-16 (malloc()ed, *wn its length), NULL on failure. */
static WCHAR *ansi_text(LPCSTR s, int n, int *wn)
{
	*wn = n > 0 ? cp_to_utf16(CP_ACP, s, n, NULL, 0, 0) : 0;
	if (*wn < 0)
		return NULL;
	WCHAR *w = malloc(((size_t)*wn + 1) * sizeof(WCHAR));
	if (w && *wn)
		cp_to_utf16(CP_ACP, s, n, w, *wn, 0);
	return w;
}

WINFN(BOOL) gdi_GetTextExtentPoint32A(HDC h, LPCSTR s, int n, LPSIZE size)
{
	int wn;
	WCHAR *w = n > 0 && s ? ansi_text(s, n, &wn) : NULL;
	BOOL ok = text_extent(h, w ? wn : n, size);
	free(w);
	return ok;
}

WINFN(BOOL) gdi_GetTextExtentPoint32W(HDC h, LPCWSTR s, int n, LPSIZE size)
{
	return text_extent(h, n, size);
}

WINFN(BOOL) gdi_ExtTextOutW(HDC hdc, int x, int y, UINT opts, const RECT *rc,
			    LPCWSTR s, UINT n, const INT *dx)
{
	TRACE("ExtTextOutW(%d, %d, %u characters)", x, y, n);
	return n <= INT32_MAX && gdi_text_out(hdc, x, y, opts, rc, s, (int)n, dx);
}

WINFN(BOOL) gdi_ExtTextOutA(HDC hdc, int x, int y, UINT opts, const RECT *rc,
			    LPCSTR s, UINT n, const INT *dx)
{
	int wn = 0;
	WCHAR *w = NULL;

	if (n > INT32_MAX || (n && (!s || !(w = ansi_text(s, (int)n, &wn)))))
		return FALSE;
	TRACE("ExtTextOutA(%d, %d, %.*s)", x, y, (int)n, s ? s : "");
	/* A double-byte string's dx is per byte; ours one per character. */
	BOOL ok = gdi_text_out(hdc, x, y, opts, rc, w, wn,
			       wn == (int)n ? dx : NULL);
	free(w);
	return ok;
}

WINFN(BOOL) gdi_TextOutA(HDC hdc, int x, int y, LPCSTR s, int n)
{
	return n >= 0 && gdi_ExtTextOutA(hdc, x, y, 0, NULL, s, (UINT)n, NULL);
}

WINFN(BOOL) gdi_TextOutW(HDC hdc, int x, int y, LPCWSTR s, int n)
{
	return n >= 0 && gdi_ExtTextOutW(hdc, x, y, 0, NULL, s, (UINT)n, NULL);
}

/*
 * Font enumeration. There is one font, which draws any face at any size, so
 * it is listed under the faces programs commonly ask for, each a scalable
 * ANSI family as a TrueType font would be.
 */
static const struct {
	const char *face;
	BYTE pitch;
} font_faces[] = {
	{ "MS Sans Serif", VARIABLE_PITCH | FF_SWISS },
	{ "Arial", VARIABLE_PITCH | FF_SWISS },
	{ "Tahoma", VARIABLE_PITCH | FF_SWISS },
	{ "Times New Roman", VARIABLE_PITCH | FF_ROMAN },
	{ "Courier New", FIXED_PITCH | FF_MODERN },
};

typedef int (CALLBACK *FONTENUMPROCW)(const LOGFONTW *, const TEXTMETRICW *,
				      DWORD, LPARAM);
typedef int (CALLBACK *FONTENUMPROCA)(const LOGFONTA *, const TEXTMETRICA *,
				      DWORD, LPARAM);

static void ascii_to_wide(WCHAR *out, const char *s, size_t n)
{
	size_t i = 0;

	for (; s[i] && i + 1 < n; i++)
		out[i] = (BYTE)s[i];
	out[i] = 0;
}

static int same_face(const WCHAR *a, const char *b)
{
	for (;; a++, b++) {
		WCHAR x = *a >= 'a' && *a <= 'z' ? *a - 32 : *a;
		char y = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
		if (x != (BYTE)y)
			return 0;
		if (!x)
			return 1;
	}
}

/* Entry i as EnumFontFamiliesExW describes it. */
static void font_entry(size_t i, ENUMLOGFONTEXW *lf, NEWTEXTMETRICEXW *tm)
{
	LOGFONTW want = { .lfHeight = 16 };
	TEXTMETRICW m;

	memset(lf, 0, sizeof(*lf));
	memset(tm, 0, sizeof(*tm));
	ascii_to_wide(want.lfFaceName, font_faces[i].face, LF_FACESIZE);
	HFONT f = gdi_CreateFontIndirectW(&want);
	HDC dc = gdi_CreateCompatibleDC(NULL);
	HGDIOBJ old = gdi_SelectObject(dc, f);
	gdi_text_metrics(dc, &m);
	gdi_SelectObject(dc, old);
	gdi_DeleteDC(dc);
	gdi_DeleteObject(f);

	BYTE pitch = font_faces[i].pitch;
	m.tmPitchAndFamily = (pitch & 0xf0) | TMPF_TRUETYPE | TMPF_VECTOR |
			     ((pitch & 3) == VARIABLE_PITCH ? TMPF_FIXED_PITCH : 0);
	/* TEXTMETRICW is NEWTEXTMETRICW's first fields. */
	memcpy(&tm->ntmTm, &m, sizeof(m));
	tm->ntmTm.tmCharSet = ANSI_CHARSET;
	tm->ntmTm.ntmFlags = NTM_REGULAR;
	tm->ntmTm.ntmSizeEM = (UINT)m.tmHeight;
	tm->ntmTm.ntmCellHeight = (UINT)m.tmHeight;
	tm->ntmTm.ntmAvgWidth = (UINT)m.tmAveCharWidth;
	tm->ntmFontSig.fsUsb[0] = 1;		/* Basic Latin */
	tm->ntmFontSig.fsCsb[0] = 1;		/* Latin 1 */

	lf->elfLogFont.lfHeight = m.tmHeight;
	lf->elfLogFont.lfWidth = m.tmAveCharWidth;
	lf->elfLogFont.lfWeight = FW_NORMAL;
	lf->elfLogFont.lfCharSet = ANSI_CHARSET;
	lf->elfLogFont.lfOutPrecision = OUT_STROKE_PRECIS;
	lf->elfLogFont.lfClipPrecision = CLIP_STROKE_PRECIS;
	lf->elfLogFont.lfQuality = DRAFT_QUALITY;
	lf->elfLogFont.lfPitchAndFamily = font_faces[i].pitch;
	ascii_to_wide(lf->elfLogFont.lfFaceName, font_faces[i].face,
		      LF_FACESIZE);
	ascii_to_wide(lf->elfFullName, font_faces[i].face, LF_FULLFACESIZE);
	ascii_to_wide(lf->elfStyle, "Regular", LF_FACESIZE);
	ascii_to_wide(lf->elfScript, "Western", LF_FACESIZE);
}

static void font_entry_a(const ENUMLOGFONTEXW *w, const NEWTEXTMETRICEXW *tw,
			 ENUMLOGFONTEXA *a, NEWTEXTMETRICEXA *ta)
{
	memset(a, 0, sizeof(*a));
	memset(ta, 0, sizeof(*ta));
	memcpy(&a->elfLogFont, &w->elfLogFont, offsetof(LOGFONTA, lfFaceName));
	/* The names are ASCII. */
	for (int i = 0; i < LF_FACESIZE; i++) {
		a->elfLogFont.lfFaceName[i] = (CHAR)w->elfLogFont.lfFaceName[i];
		a->elfStyle[i] = (BYTE)w->elfStyle[i];
		a->elfScript[i] = (BYTE)w->elfScript[i];
	}
	for (int i = 0; i < LF_FULLFACESIZE; i++)
		a->elfFullName[i] = (BYTE)w->elfFullName[i];

	const NEWTEXTMETRICW *m = &tw->ntmTm;
	memcpy(&ta->ntmTm, m, offsetof(NEWTEXTMETRICA, tmFirstChar));
	ta->ntmTm.tmFirstChar = (BYTE)m->tmFirstChar;
	ta->ntmTm.tmLastChar = (BYTE)m->tmLastChar;
	ta->ntmTm.tmDefaultChar = (BYTE)m->tmDefaultChar;
	ta->ntmTm.tmBreakChar = (BYTE)m->tmBreakChar;
	ta->ntmTm.tmItalic = m->tmItalic;
	ta->ntmTm.tmUnderlined = m->tmUnderlined;
	ta->ntmTm.tmStruckOut = m->tmStruckOut;
	ta->ntmTm.tmPitchAndFamily = m->tmPitchAndFamily;
	ta->ntmTm.tmCharSet = m->tmCharSet;
	ta->ntmTm.ntmFlags = m->ntmFlags;
	ta->ntmTm.ntmSizeEM = m->ntmSizeEM;
	ta->ntmTm.ntmCellHeight = m->ntmCellHeight;
	ta->ntmTm.ntmAvgWidth = m->ntmAvgWidth;
	ta->ntmFontSig = tw->ntmFontSig;
}

/*
 * Calls procw or proca for each face matching face (all if NULL or empty)
 * in charset (any if DEFAULT_CHARSET); the last value it returned, or 1 if
 * nothing matched. The LOGFONT and TEXTMETRIC each get are the start of
 * an ENUMLOGFONTEX and NEWTEXTMETRICEX, as for every EnumFont* function.
 */
static int enum_fonts(const WCHAR *face, BYTE charset, FONTENUMPROCW procw,
		      FONTENUMPROCA proca, LPARAM lp)
{
	int ret = 1;

	if (charset != DEFAULT_CHARSET && charset != ANSI_CHARSET)
		return ret;
	for (size_t i = 0; i < sizeof(font_faces) / sizeof(font_faces[0]); i++) {
		ENUMLOGFONTEXW lf;
		NEWTEXTMETRICEXW tm;

		if (face && face[0] && !same_face(face, font_faces[i].face))
			continue;
		font_entry(i, &lf, &tm);
		TRACE("font enumeration: %s", font_faces[i].face);
		if (procw) {
			ret = procw(&lf.elfLogFont, (const TEXTMETRICW *)&tm,
				    TRUETYPE_FONTTYPE, lp);
		} else {
			ENUMLOGFONTEXA lfa;
			NEWTEXTMETRICEXA tma;
			font_entry_a(&lf, &tm, &lfa, &tma);
			ret = proca(&lfa.elfLogFont, (const TEXTMETRICA *)&tma,
				    TRUETYPE_FONTTYPE, lp);
		}
		if (!ret)
			break;
	}
	return ret;
}

/* An ANSI face name as UTF-16, "" for NULL. */
static void face_w(LPCSTR face, WCHAR out[LF_FACESIZE])
{
	out[0] = 0;
	if (face && face[0])
		cp_to_utf16(CP_ACP, face, -1, out, LF_FACESIZE, 0);
	out[LF_FACESIZE - 1] = 0;
}

WINFN(int) gdi_EnumFontFamiliesExW(HDC hdc, const LOGFONTW *lf,
				   FONTENUMPROCW proc, LPARAM lp, DWORD flags)
{
	if (!proc)
		return 0;
	return enum_fonts(lf ? lf->lfFaceName : NULL,
			  lf ? lf->lfCharSet : DEFAULT_CHARSET, proc, NULL, lp);
}

WINFN(int) gdi_EnumFontFamiliesExA(HDC hdc, const LOGFONTA *lf,
				   FONTENUMPROCA proc, LPARAM lp, DWORD flags)
{
	WCHAR face[LF_FACESIZE];

	if (!proc)
		return 0;
	face_w(lf ? lf->lfFaceName : NULL, face);
	return enum_fonts(face, lf ? lf->lfCharSet : DEFAULT_CHARSET, NULL,
			  proc, lp);
}

WINFN(int) gdi_EnumFontFamiliesW(HDC hdc, LPCWSTR face, FONTENUMPROCW proc,
				 LPARAM lp)
{
	return proc ? enum_fonts(face, DEFAULT_CHARSET, proc, NULL, lp) : 0;
}

WINFN(int) gdi_EnumFontFamiliesA(HDC hdc, LPCSTR face, FONTENUMPROCA proc,
				 LPARAM lp)
{
	WCHAR w[LF_FACESIZE];

	face_w(face, w);
	return proc ? enum_fonts(w, DEFAULT_CHARSET, NULL, proc, lp) : 0;
}

static BOOL dc_point(HDC hdc, POINT *old, int x, int y, int which, int offset)
{
	gdi_lock();
	struct gdi_obj *o = gdi_get(hdc, OBJ_DC);
	if (!o) { gdi_unlock(); return FALSE; }
	POINT *p = which ? &o->u.dc.s.viewport_org : (POINT *)&o->u.dc.s.viewport_ext;
	if (old) *old = *p;
	if (offset) { p->x += x; p->y += y; } else { p->x = x; p->y = y; }
	gdi_unlock(); return TRUE;
}
WINFN(BOOL) gdi_OffsetViewportOrgEx(HDC h, int x, int y, LPPOINT old) { return dc_point(h, old, x, y, 1, 1); }
WINFN(BOOL) gdi_SetViewportOrgEx(HDC h, int x, int y, LPPOINT old) { return dc_point(h, old, x, y, 1, 0); }
WINFN(BOOL) gdi_SetViewportExtEx(HDC h, int x, int y, LPSIZE old) { return dc_point(h, (POINT *)old, x, y, 0, 0); }
WINFN(BOOL) gdi_SetWindowExtEx(HDC h, int x, int y, LPSIZE old)
{
	gdi_lock(); struct gdi_obj *o = gdi_get(h, OBJ_DC);
	if (!o) { gdi_unlock(); return FALSE; }
	if (old) *old = o->u.dc.s.window_ext;
	o->u.dc.s.window_ext = (SIZE){x, y}; gdi_unlock(); return TRUE;
}
static BOOL scale_ext(HDC h, int xn, int xd, int yn, int yd, LPSIZE old, int viewport)
{
	if (!xd || !yd)
		return FALSE;
	gdi_lock();
	struct gdi_obj *o = gdi_get(h, OBJ_DC);
	if (!o) {
		gdi_unlock();
		return FALSE;
	}
	SIZE *s = viewport ? &o->u.dc.s.viewport_ext : &o->u.dc.s.window_ext;
	if (old)
		*old = *s;
	s->cx = (LONG)((long long)s->cx * xn / xd);
	s->cy = (LONG)((long long)s->cy * yn / yd);
	gdi_unlock();
	return TRUE;
}
WINFN(BOOL) gdi_ScaleViewportExtEx(HDC h, int xn, int xd, int yn, int yd,
	LPSIZE old)
{
	return scale_ext(h, xn, xd, yn, yd, old, 1);
}
WINFN(BOOL) gdi_ScaleWindowExtEx(HDC h, int xn, int xd, int yn, int yd,
	LPSIZE old)
{
	return scale_ext(h, xn, xd, yn, yd, old, 0);
}
WINFN(int) gdi_GetClipBox(HDC h, LPRECT r)
{
	gdi_lock();
	BOOL ok = gdi_get(h, OBJ_DC) != NULL;
	gdi_unlock();
	if (!ok || !r)
		return 0;
	*r = (RECT){ 0, 0, GDI_SCREEN_WIDTH, GDI_SCREEN_HEIGHT };
	return 2;
}
WINFN(BOOL) gdi_PtVisible(HDC h, int x, int y) { RECT r; return gdi_GetClipBox(h,&r) && x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom; }
WINFN(BOOL) gdi_RectVisible(HDC h, const RECT *r) { RECT c; return r&&gdi_GetClipBox(h,&c)&&r->right>c.left&&r->left<c.right&&r->bottom>c.top&&r->top<c.bottom; }
WINFN(BOOL) gdi_Rectangle(HDC h,int l,int t,int r,int b) { (void)l;(void)t;(void)r;(void)b; gdi_lock(); BOOL ok=gdi_get(h,OBJ_DC)!=NULL; gdi_unlock(); return ok; }
WINFN(int) gdi_Escape(HDC h,int esc,int n,LPCSTR in,LPVOID out) { (void)esc;(void)n;(void)in;(void)out; gdi_lock(); BOOL ok=gdi_get(h,OBJ_DC)!=NULL; gdi_unlock(); return ok ? 0 : -1; }

#define E(name) { #name, 0, (void *)gdi_##name }

static const struct builtin_export exports[] = {
	E(CreateBrushIndirect),
	E(CreateCompatibleDC),
	E(CreateDCA),
	E(CreateDCW),
	E(CreateFontA),
	E(CreateFontIndirectA),
	E(CreateFontIndirectW),
	E(CreateFontW),
	E(CreateHalftonePalette),
	{ "CreateICA", 0, (void *)gdi_CreateDCA },
	{ "CreateICW", 0, (void *)gdi_CreateDCW },
	E(CreatePalette),
	E(CreatePatternBrush),
	E(CreatePen),
	E(CreatePenIndirect),
	E(CreateSolidBrush),
	E(DeleteDC),
	E(DeleteEnhMetaFile),
	E(DeleteObject),
	E(EnumFontFamiliesA),
	E(EnumFontFamiliesExA),
	E(EnumFontFamiliesExW),
	E(EnumFontFamiliesW),
	{ "EnumFontsA", 0, (void *)gdi_EnumFontFamiliesA },
	{ "EnumFontsW", 0, (void *)gdi_EnumFontFamiliesW },
	E(ExtTextOutA),
	E(ExtTextOutW),
	E(Escape),
	E(GdiFlush),
	E(GetBkColor),
	E(GetBkMode),
	E(GetClipBox),
	E(GetCurrentObject),
	E(GetDeviceCaps),
	E(GetMapMode),
	E(GetNearestColor),
	E(GetNearestPaletteIndex),
	E(GetObjectA),
	E(GetObjectType),
	E(GetObjectW),
	E(GetPaletteEntries),
	E(GetROP2),
	E(GetStockObject),
	E(GetStretchBltMode),
	E(GetSystemPaletteEntries),
	E(GetSystemPaletteUse),
	E(GetTextAlign),
	E(GetTextColor),
	{ "GetTextExtentPointA", 0, (void *)gdi_GetTextExtentPoint32A },
	E(GetTextExtentPoint32A),
	E(GetTextExtentPoint32W),
	{ "GetTextExtentPointW", 0, (void *)gdi_GetTextExtentPoint32W },
	E(GetTextMetricsA),
	E(GetTextMetricsW),
	E(MoveToEx),
	E(OffsetViewportOrgEx),
	E(PtVisible),
	E(PlayEnhMetaFile),
	E(RectVisible),
	E(Rectangle),
	E(RealizePalette),
	E(RestoreDC),
	E(SaveDC),
	E(SelectObject),
	E(SelectPalette),
	E(SetBkColor),
	E(SetEnhMetaFileBits),
	E(SetBkMode),
	E(SetMapMode),
	E(SetPaletteEntries),
	E(SetROP2),
	E(SetStretchBltMode),
	E(SetTextAlign),
	E(SetTextColor),
	E(ScaleViewportExtEx),
	E(ScaleWindowExtEx),
	E(SetViewportExtEx),
	E(SetViewportOrgEx),
	E(SetWindowExtEx),
	E(TextOutA),
	E(TextOutW),
	BUILTIN_END
};

extern const struct builtin_export gdi_bitmap_exports[];

static const struct builtin_export *const tables[] = {
	exports, gdi_bitmap_exports, NULL
};

const struct builtin_dll builtin_gdi32 = { "gdi32.dll", NULL, tables, 0 };
