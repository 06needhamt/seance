/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gdi32 internals, shared between its source files and with user32 (whose
 * GetDC hands out display DCs, and whose FillRect paints with brushes).
 *
 * GDI objects live in one handle table under one (recursive) lock. A
 * handle carries the object's type in bits 16-22 as on Windows (0x01 DC,
 * 0x05 bitmap, 0x08 palette, 0x0a font, 0x10 brush, 0x30 pen) and a
 * generation count in its top byte, so a stale handle is caught.
 *
 * There is no screen: a display DC (GetDC, CreateDC("DISPLAY")) reports a
 * 32-bit true-colour device, drawing to it is discarded and reading from
 * it gives black. Memory DCs draw into the bitmap selected into them.
 */
#ifndef VFW_WIN32_GDI_H
#define VFW_WIN32_GDI_H

#include "win32.h"

/* The pretend display. */
#define GDI_SCREEN_WIDTH  1024
#define GDI_SCREEN_HEIGHT 768
#define GDI_SCREEN_BPP    32

struct gdi_bitmap {
	int width, height;	/* height > 0 */
	int top_down;
	int bpp;
	int stride;		/* bytes per row */
	BYTE *bits;
	int dib;		/* a DIB section (else a device-dependent bitmap) */
	int owns_bits;		/* bits were allocated here (else mapped) */
	void *view;		/* the file-mapping view holding bits, if any */
	DWORD compression;	/* BI_RGB or BI_BITFIELDS */
	DWORD masks[3];		/* red, green, blue (16 and 32 bpp) */
	RGBQUAD colors[256];	/* <= 8 bpp */
	int ncolors;
	HANDLE section;
	DWORD offset;
	HDC selected;		/* the DC it is selected into */
};

struct gdi_palette {
	WORD n;
	PALETTEENTRY *entries;
};

struct gdi_dc_state {
	HGDIOBJ bitmap, brush, pen, font;
	HPALETTE palette;
	COLORREF text, bk;
	int bkmode, stretch_mode, map_mode, rop2;
	UINT text_align;
	POINT pos;
	POINT viewport_org;
	SIZE viewport_ext, window_ext;
};

struct gdi_dc {
	int memory;		/* a memory DC (else the display) */
	HWND hwnd;		/* the window of a GetDC DC */
	struct gdi_dc_state s;
	struct gdi_dc_state *saved;	/* SaveDC's stack */
	int nsaved;
};

struct gdi_obj {
	int type;		/* OBJ_* */
	int stock;		/* never deleted */
	HGDIOBJ handle;
	union {
		struct gdi_bitmap bmp;
		struct gdi_palette pal;
		LOGBRUSH brush;
		LOGPEN pen;
		LOGFONTW font;
		struct { UINT size; BYTE *bits; } emf;
		struct gdi_dc dc;
	} u;
};

void gdi_lock(void);
void gdi_unlock(void);

/* The object behind h if it has type (0 for any), with the lock held. */
struct gdi_obj *gdi_get(HGDIOBJ h, int type);

/* A new object of type, zeroed, with its handle; NULL if full. */
struct gdi_obj *gdi_new(int type);
/* Frees an object (and whatever it owns) and its handle. */
void gdi_free(struct gdi_obj *o);

/* A display DC for hwnd (GetDC), and releasing one (ReleaseDC). */
HDC gdi_display_dc(HWND hwnd);
int gdi_release_dc(HWND hwnd, HDC hdc);

/* Fills rc on hdc with brush (FillRect); returns non-zero on success. */
int gdi_fill_rect(HDC hdc, const RECT *rc, HBRUSH brush);

/* A brush for a system colour (GetSysColorBrush): a stock-like object. */
HBRUSH gdi_sys_color_brush(int index, COLORREF color);

/* The palettes: the 20 static system colours and the halftone palette. */
extern const PALETTEENTRY gdi_default_palette[20];
void gdi_halftone_palette(PALETTEENTRY out[256]);

/* gdi_bitmap.c: freeing a bitmap's memory. */
void gdi_bitmap_free(struct gdi_bitmap *b);
/* The stock 1x1 monochrome bitmap a new memory DC holds. */
HBITMAP gdi_default_bitmap(void);
/* Sets one up. */
void gdi_bitmap_init_default(struct gdi_bitmap *b);
/* The colour of index i in a DC's palette. */
COLORREF gdi_palette_color(const struct gdi_dc *dc, WORD i);

#endif /* VFW_WIN32_GDI_H */
