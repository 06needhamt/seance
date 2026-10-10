/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gdi32's text: one bitmap font (gdi_font.h, X11's misc-fixed 8x13) scaled
 * to whatever a LOGFONT asks for, drawn into memory DCs' bitmaps by
 * TextOut, ExtTextOut and user32's DrawText, and measured the same way.
 *
 * A font's cell is its height (16 pixels for 0; a negative height is the
 * characters' own, and the cell adds an eighth for accents). Its
 * characters are the glyphs scaled to the cell, all as wide as the cell is
 * high times 8/13 unless lfWidth says otherwise. Bold thickens strokes by
 * a pixel, italic slants them, and underline and strikeout are lines
 * across the run. A character the font lacks (beyond ASCII) is drawn as
 * its '?'. Text goes where SetTextAlign says, over the background colour
 * in OPAQUE mode, and ExtTextOut's ETO_OPAQUE and ETO_CLIPPED apply.
 */
#include <stdlib.h>
#include <string.h>

#include "win32/gdi.h"
#include "win32/gdi_font.h"
#include "win32/internal.h"

/* Larger runs are measured but not drawn. */
#define MAX_PIXELS (64u << 20)

struct font {
	int cell, ascent, internal, width;
	int weight, bold, italic, underline, strike;
	BYTE charset, pitch;
};

/*
 * The DC's font, and its state if st; 0 if hdc is no DC. Measuring with no
 * DC at all (st NULL) gives the system font's.
 */
static int dc_text(HDC hdc, struct font *f, struct gdi_dc_state *st)
{
	LOGFONTW lf = { .lfHeight = 16 };
	int ok = !hdc && !st;

	gdi_lock();
	struct gdi_obj *d = hdc ? gdi_get(hdc, OBJ_DC) : NULL;
	struct gdi_obj *fo = d ? gdi_get(d->u.dc.s.font, OBJ_FONT) : NULL;
	if (fo) {
		lf = fo->u.font;
		if (st)
			*st = d->u.dc.s;
		ok = 1;
	}
	gdi_unlock();
	if (!ok)
		return 0;

	int h = lf.lfHeight < 0 ? -lf.lfHeight : lf.lfHeight;
	if (!h)
		h = 16;
	f->internal = lf.lfHeight < 0 ? h / 8 : 0;
	f->cell = h + f->internal;
	f->ascent = (f->cell * GDI_FONT_ASCENT + GDI_FONT_HEIGHT / 2) /
		    GDI_FONT_HEIGHT;
	f->width = lf.lfWidth ? abs(lf.lfWidth) :
		   (f->cell * GDI_FONT_WIDTH + GDI_FONT_HEIGHT / 2) /
		   GDI_FONT_HEIGHT;
	if (f->width < 1)
		f->width = 1;
	f->weight = lf.lfWeight ? lf.lfWeight : FW_NORMAL;
	f->bold = f->weight >= 600;
	f->italic = lf.lfItalic;
	f->underline = lf.lfUnderline;
	f->strike = lf.lfStrikeOut;
	f->charset = lf.lfCharSet;
	f->pitch = lf.lfPitchAndFamily;
	return 1;
}

int gdi_text_metrics(HDC hdc, TEXTMETRICW *tm)
{
	struct font f;

	if (!tm || !dc_text(hdc, &f, NULL))
		return 0;
	memset(tm, 0, sizeof(*tm));
	tm->tmHeight = f.cell;
	tm->tmAscent = f.ascent;
	tm->tmDescent = f.cell - f.ascent;
	tm->tmInternalLeading = f.internal;
	tm->tmAveCharWidth = f.width;
	tm->tmMaxCharWidth = f.width;
	tm->tmWeight = f.weight;
	tm->tmDigitizedAspectX = tm->tmDigitizedAspectY = 96;
	tm->tmFirstChar = 0x20;
	tm->tmLastChar = 0xff;
	tm->tmDefaultChar = '?';
	tm->tmBreakChar = ' ';
	tm->tmItalic = (BYTE)f.italic;
	tm->tmUnderlined = (BYTE)f.underline;
	tm->tmStruckOut = (BYTE)f.strike;
	tm->tmPitchAndFamily = f.pitch;
	tm->tmCharSet = f.charset;
	return 1;
}

int gdi_text_width(HDC hdc, int n, int *width, int *height)
{
	struct font f;

	if (n < 0 || !dc_text(hdc, &f, NULL))
		return 0;
	if (width)
		*width = n * f.width;
	if (height)
		*height = f.cell;
	return 1;
}

static const unsigned char *glyph(WCHAR c)
{
	if (c < GDI_FONT_FIRST || c > GDI_FONT_LAST)
		c = '?';
	return gdi_font[c - GDI_FONT_FIRST];
}

/* Whether (px, py) of a character's cell is ink, before bold. */
static int ink(const struct font *f, const unsigned char *g, int px, int py)
{
	/* Italic leans the top right, a quarter of the way up. */
	if (f->italic)
		px -= (f->ascent - py) / 4;
	if (px < 0 || px >= f->width || py < 0 || py >= f->cell)
		return 0;
	int sx = px * GDI_FONT_WIDTH / f->width;
	int sy = py * GDI_FONT_HEIGHT / f->cell;
	return (g[sy] >> (7 - sx)) & 1;
}

/* The run's ink: w x f->cell bytes. */
static void render(const struct font *f, const WCHAR *s, int n, const INT *dx,
		   BYTE *mask, int w)
{
	int ox = 0;

	for (int i = 0; i < n; i++) {
		const unsigned char *g = glyph(s[i]);
		for (int py = 0; py < f->cell; py++)
			for (int px = 0; px < f->width + f->bold; px++) {
				int x = ox + px;
				if (x < 0 || x >= w)
					continue;
				if (ink(f, g, px, py) ||
				    (f->bold && ink(f, g, px - 1, py)))
					mask[(size_t)py * w + x] = 1;
			}
		int advance = dx ? dx[i] : f->width;
		ox += advance > 0 ? advance : 0;
	}
	int thick = f->cell / 14 > 1 ? f->cell / 14 : 1;
	int lines[2] = {
		f->underline ? f->ascent + 1 : -1,
		f->strike ? f->ascent - f->ascent / 3 : -1,
	};
	for (int l = 0; l < 2; l++)
		for (int t = 0; lines[l] >= 0 && t < thick; t++)
			if (lines[l] + t < f->cell)
				memset(mask + (size_t)(lines[l] + t) * w, 1, w);
}

int gdi_text_out(HDC hdc, int x, int y, UINT opts, const RECT *rc,
		 const WCHAR *s, int n, const INT *dx)
{
	struct font f;
	struct gdi_dc_state st;

	if (n < 0 || (n && !s) || !dc_text(hdc, &f, &st))
		return 0;
	long long width = 0;
	for (int i = 0; i < n; i++)
		width += dx ? (dx[i] > 0 ? dx[i] : 0) : f.width;

	UINT align = st.text_align;
	if (align & TA_UPDATECP) {
		x = st.pos.x;
		y = st.pos.y;
	}
	int left = x;
	if ((align & TA_CENTER) == TA_CENTER)
		left = x - (int)(width / 2);
	else if (align & TA_RIGHT)
		left = x - (int)width;
	int top = (align & TA_BASELINE) == TA_BASELINE ? y - f.ascent :
		  (align & TA_BOTTOM) ? y - f.cell : y;
	const RECT *clip = (opts & ETO_CLIPPED) && rc ? rc : NULL;

	if ((opts & ETO_OPAQUE) && rc)
		gdi_fill_color(hdc, rc, st.bk, NULL);
	if (n && width > 0 && (unsigned long long)width * f.cell <= MAX_PIXELS) {
		int w = (int)width + f.bold;
		BYTE *mask = calloc((size_t)w * f.cell, 1);
		if (!mask)
			return 0;
		render(&f, s, n, dx, mask, w);
		gdi_paint_mask(hdc, left, top, w, f.cell, mask, st.text,
			       st.bkmode == OPAQUE ? &st.bk : NULL, clip);
		free(mask);
	}

	/* The current position moves past the text, as it was written. */
	if ((align & TA_UPDATECP) && (align & TA_CENTER) != TA_CENTER) {
		gdi_lock();
		struct gdi_obj *d = gdi_get(hdc, OBJ_DC);
		if (d)
			d->u.dc.s.pos.x = align & TA_RIGHT ? x - (int)width :
						 x + (int)width;
		gdi_unlock();
	}
	return 1;
}
