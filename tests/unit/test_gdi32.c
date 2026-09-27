/*
 * gdi32, called through the export table the way a DLL's imports reach it:
 * objects and DCs, DIB sections and conversions, blits, palettes, text.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "loader/modules.h"
#include "runtime/lowmem.h"
#include "runtime/runtime.h"
#include "test.h"

static struct module *gdi, *k32;

static void *get_from(struct module *m, const char *name)
{
	void *p = mod_get_proc(m, name);
	if (!p) {
		fprintf(stderr, "%s has no %s\n", m->name, name);
		exit(1);
	}
	return p;
}

#define FN(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = \
		(ret (WINAPI *)(__VA_ARGS__))get_from(gdi, #name)
#define K32(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = \
		(ret (WINAPI *)(__VA_ARGS__))get_from(k32, #name)

/* A BITMAPINFO with room for a full colour table or masks. */
struct bmi {
	BITMAPINFOHEADER h;
	RGBQUAD colors[256];
};

static void header(struct bmi *b, int w, int h, int bpp, DWORD compression)
{
	memset(b, 0, sizeof(*b));
	b->h.biSize = sizeof(b->h);
	b->h.biWidth = w;
	b->h.biHeight = h;
	b->h.biPlanes = 1;
	b->h.biBitCount = bpp;
	b->h.biCompression = compression;
}

static void test_objects(void)
{
	FN(HGDIOBJ, GetStockObject, int);
	FN(DWORD, GetObjectType, HGDIOBJ);
	FN(int, GetObjectA, HGDIOBJ, int, LPVOID);
	FN(int, GetObjectW, HGDIOBJ, int, LPVOID);
	FN(BOOL, DeleteObject, HGDIOBJ);
	FN(HBRUSH, CreateSolidBrush, COLORREF);
	FN(HPEN, CreatePen, int, int, COLORREF);
	FN(HFONT, CreateFontA, int, int, int, int, int, DWORD, DWORD, DWORD,
	   DWORD, DWORD, DWORD, DWORD, DWORD, LPCSTR);

	HGDIOBJ white = GetStockObject(WHITE_BRUSH);
	CHECK(white != NULL);
	CHECK_EQ(((ULONG_PTR)white >> 16) & 0x7f, 0x10);
	CHECK_EQ(GetObjectType(white), OBJ_BRUSH);
	CHECK_EQ(GetObjectType(GetStockObject(BLACK_PEN)), OBJ_PEN);
	CHECK_EQ(GetObjectType(GetStockObject(SYSTEM_FONT)), OBJ_FONT);
	CHECK_EQ(GetObjectType(GetStockObject(DEFAULT_PALETTE)), OBJ_PAL);
	CHECK(GetStockObject(9) == NULL);
	CHECK(GetStockObject(20) == NULL);
	CHECK(DeleteObject(white));	/* stock: a no-op */
	CHECK_EQ(GetObjectType(white), OBJ_BRUSH);

	LOGBRUSH lb;
	HBRUSH br = CreateSolidBrush(RGB(1, 2, 3));
	CHECK_EQ(GetObjectA(br, sizeof(lb), &lb), sizeof(lb));
	CHECK_EQ(lb.lbStyle, BS_SOLID);
	CHECK_EQ(lb.lbColor, RGB(1, 2, 3));
	CHECK(DeleteObject(br));
	CHECK_EQ(GetObjectType(br), 0);
	CHECK(!DeleteObject(br));
	/* The slot is reused, but the old handle stays dead. */
	HBRUSH br2 = CreateSolidBrush(0);
	CHECK(br2 != br);
	CHECK_EQ(GetObjectType(br), 0);
	DeleteObject(br2);

	LOGPEN lp;
	HPEN pen = CreatePen(PS_SOLID, 3, RGB(9, 9, 9));
	CHECK_EQ(GetObjectA(pen, sizeof(lp), &lp), sizeof(lp));
	CHECK_EQ(lp.lopnWidth.x, 3);
	DeleteObject(pen);

	HFONT f = CreateFontA(-12, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0,
			      0, 0, "Arial");
	LOGFONTA lfa;
	LOGFONTW lfw;
	CHECK_EQ(GetObjectA(f, sizeof(lfa), &lfa), sizeof(lfa));
	CHECK_STR(lfa.lfFaceName, "Arial");
	CHECK_EQ(lfa.lfWeight, FW_BOLD);
	CHECK_EQ(GetObjectW(f, sizeof(lfw), &lfw), sizeof(lfw));
	CHECK(lfw.lfFaceName[0] == 'A' && lfw.lfFaceName[5] == 0);
	CHECK_EQ(GetObjectA(f, 0, NULL), sizeof(LOGFONTA));
	DeleteObject(f);
}

static void test_dc_state(void)
{
	FN(HDC, CreateCompatibleDC, HDC);
	FN(HDC, CreateDCA, LPCSTR, LPCSTR, LPCSTR, const void *);
	FN(BOOL, DeleteDC, HDC);
	FN(int, GetDeviceCaps, HDC, int);
	FN(COLORREF, SetTextColor, HDC, COLORREF);
	FN(COLORREF, GetTextColor, HDC);
	FN(int, SetBkMode, HDC, int);
	FN(int, SaveDC, HDC);
	FN(BOOL, RestoreDC, HDC, int);
	FN(HGDIOBJ, SelectObject, HDC, HGDIOBJ);
	FN(HGDIOBJ, GetCurrentObject, HDC, UINT);
	FN(HBRUSH, CreateSolidBrush, COLORREF);
	FN(BOOL, DeleteObject, HGDIOBJ);
	FN(HGDIOBJ, GetStockObject, int);
	FN(BOOL, GetTextMetricsA, HDC, LPTEXTMETRICA);
	FN(BOOL, GetTextExtentPoint32A, HDC, LPCSTR, int, LPSIZE);
	FN(BOOL, TextOutA, HDC, int, int, LPCSTR, int);
	FN(DWORD, GetObjectType, HGDIOBJ);

	HDC disp = CreateDCA("DISPLAY", NULL, NULL, NULL);
	CHECK(disp != NULL);
	CHECK_EQ(GetObjectType(disp), OBJ_DC);
	CHECK_EQ(GetDeviceCaps(disp, BITSPIXEL), 32);
	CHECK_EQ(GetDeviceCaps(disp, HORZRES), 1024);
	CHECK_EQ(GetDeviceCaps(disp, LOGPIXELSY), 96);
	CHECK(!(GetDeviceCaps(disp, RASTERCAPS) & RC_PALETTE));
	CHECK(CreateDCA("WINSPOOL", "printer", NULL, NULL) == NULL);

	HDC mem = CreateCompatibleDC(disp);
	CHECK_EQ(GetObjectType(mem), OBJ_MEMDC);
	CHECK_EQ(SetTextColor(mem, RGB(1, 1, 1)), RGB(0, 0, 0));
	CHECK_EQ(SetBkMode(mem, TRANSPARENT), OPAQUE);
	HBRUSH br = CreateSolidBrush(RGB(5, 5, 5));
	CHECK_EQ(SaveDC(mem), 1);
	CHECK(SelectObject(mem, br) == GetStockObject(WHITE_BRUSH));
	SetTextColor(mem, RGB(2, 2, 2));
	CHECK_EQ(SaveDC(mem), 2);
	SetTextColor(mem, RGB(3, 3, 3));
	CHECK(RestoreDC(mem, -1));
	CHECK_EQ(GetTextColor(mem), RGB(2, 2, 2));
	CHECK(RestoreDC(mem, 1));
	CHECK_EQ(GetTextColor(mem), RGB(1, 1, 1));
	CHECK(GetCurrentObject(mem, OBJ_BRUSH) == GetStockObject(WHITE_BRUSH));
	CHECK(!RestoreDC(mem, 1));
	CHECK(DeleteObject(br));

	/* A selected brush can't be deleted. */
	br = CreateSolidBrush(0);
	SelectObject(mem, br);
	CHECK(!DeleteObject(br));
	SelectObject(mem, GetStockObject(BLACK_BRUSH));
	CHECK(DeleteObject(br));

	TEXTMETRICA tm;
	SIZE size;
	CHECK(GetTextMetricsA(mem, &tm));
	CHECK_EQ(tm.tmHeight, 16);
	CHECK_EQ(tm.tmAscent + tm.tmDescent, 16);
	CHECK(GetTextExtentPoint32A(mem, "abcd", 4, &size));
	CHECK_EQ(size.cx, 4 * tm.tmAveCharWidth);
	CHECK_EQ(size.cy, 16);
	CHECK(TextOutA(mem, 0, 0, "x", 1));

	CHECK(DeleteDC(mem));
	CHECK(!DeleteDC(mem));
	CHECK(DeleteDC(disp));
}

static void test_dib_section(void)
{
	FN(HDC, CreateCompatibleDC, HDC);
	FN(BOOL, DeleteDC, HDC);
	FN(HBITMAP, CreateDIBSection, HDC, const BITMAPINFO *, UINT, void **,
	   HANDLE, DWORD);
	FN(HGDIOBJ, SelectObject, HDC, HGDIOBJ);
	FN(BOOL, DeleteObject, HGDIOBJ);
	FN(int, GetObjectA, HGDIOBJ, int, LPVOID);
	FN(COLORREF, SetPixel, HDC, int, int, COLORREF);
	FN(COLORREF, GetPixel, HDC, int, int);
	FN(HBITMAP, CreateCompatibleBitmap, HDC, int, int);
	FN(UINT, GetDIBColorTable, HDC, UINT, UINT, RGBQUAD *);
	FN(UINT, SetDIBColorTable, HDC, UINT, UINT, const RGBQUAD *);

	struct bmi bi;
	BYTE *bits = NULL;
	header(&bi, 5, 3, 24, BI_RGB);
	HBITMAP bm = CreateDIBSection(NULL, (BITMAPINFO *)&bi, DIB_RGB_COLORS,
				      (void **)&bits, NULL, 0);
	CHECK(bm != NULL && bits != NULL);
	CHECK_EQ((uintptr_t)bits % 4096, 0);
	if (lowmem_active())
		CHECK((uintptr_t)bits < 0x80000000u);

	DIBSECTION ds;
	CHECK_EQ(GetObjectA(bm, sizeof(ds), &ds), sizeof(ds));
	CHECK_EQ(ds.dsBm.bmWidth, 5);
	CHECK_EQ(ds.dsBm.bmHeight, 3);
	CHECK_EQ(ds.dsBm.bmWidthBytes, 16);
	CHECK_EQ(ds.dsBm.bmBitsPixel, 24);
	CHECK(ds.dsBm.bmBits == bits);
	CHECK_EQ(ds.dsBmih.biHeight, 3);
	CHECK_EQ(ds.dsBmih.biSizeImage, 48);
	BITMAP b;
	CHECK_EQ(GetObjectA(bm, sizeof(b), &b), sizeof(b));

	/* A new memory DC holds a 1x1 monochrome bitmap. */
	HDC dc = CreateCompatibleDC(NULL);
	HBITMAP def = SelectObject(dc, bm);
	CHECK(def != NULL);
	CHECK_EQ(GetObjectA(def, sizeof(b), &b), sizeof(b));
	CHECK(b.bmWidth == 1 && b.bmHeight == 1 && b.bmBitsPixel == 1);

	/* Pixels land bottom-up in the bits. */
	CHECK_EQ(SetPixel(dc, 1, 0, RGB(10, 20, 30)), RGB(10, 20, 30));
	CHECK(bits[2 * 16 + 3] == 30 && bits[2 * 16 + 4] == 20 &&
	      bits[2 * 16 + 5] == 10);
	CHECK_EQ(GetPixel(dc, 1, 0), RGB(10, 20, 30));
	CHECK_EQ(GetPixel(dc, 5, 0), CLR_INVALID);

	/* One DC at a time; a selected bitmap can't be deleted. */
	HDC dc2 = CreateCompatibleDC(NULL);
	CHECK(SelectObject(dc2, bm) == NULL);
	CHECK(!DeleteObject(bm));

	/* Compatible with a memory DC means the selected bitmap's format. */
	HBITMAP compat = CreateCompatibleBitmap(dc, 4, 4);
	CHECK_EQ(GetObjectA(compat, sizeof(b), &b), sizeof(b));
	CHECK_EQ(b.bmBitsPixel, 24);
	CHECK(b.bmBits == NULL);
	DeleteObject(compat);
	compat = CreateCompatibleBitmap(dc2, 4, 4);
	GetObjectA(compat, sizeof(b), &b);
	CHECK_EQ(b.bmBitsPixel, 1);
	DeleteObject(compat);

	SelectObject(dc, def);
	CHECK(DeleteObject(bm));

	/* 8 bits: the colour table, and nearest-colour mapping. */
	header(&bi, 4, -2, 8, BI_RGB);
	bi.h.biClrUsed = 3;
	bi.colors[0] = (RGBQUAD){ 0, 0, 0, 0 };
	bi.colors[1] = (RGBQUAD){ 0, 0, 255, 0 };	/* red */
	bi.colors[2] = (RGBQUAD){ 255, 0, 0, 0 };	/* blue */
	bm = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS,
			      (void **)&bits, NULL, 0);
	CHECK(bm != NULL);
	SelectObject(dc, bm);
	CHECK_EQ(SetPixel(dc, 0, 0, RGB(200, 10, 10)), RGB(255, 0, 0));
	CHECK_EQ(bits[0], 1);	/* top-down: row 0 first */
	SetPixel(dc, 3, 1, RGB(0, 0, 250));
	CHECK_EQ(bits[4 + 3], 2);
	RGBQUAD q[4];
	CHECK_EQ(GetDIBColorTable(dc, 0, 4, q), 4);
	CHECK(q[1].rgbRed == 255);
	RGBQUAD green = { 0, 255, 0, 0 };
	CHECK_EQ(SetDIBColorTable(dc, 1, 1, &green), 1);
	CHECK_EQ(GetPixel(dc, 0, 0), RGB(0, 255, 0));
	SelectObject(dc, def);
	DeleteObject(bm);

	/* 16 bits with 5-6-5 masks. */
	header(&bi, 2, 1, 16, BI_BITFIELDS);
	DWORD *masks = (DWORD *)bi.colors;
	masks[0] = 0xf800;
	masks[1] = 0x07e0;
	masks[2] = 0x001f;
	bm = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS,
			      (void **)&bits, NULL, 0);
	CHECK(bm != NULL);
	SelectObject(dc, bm);
	SetPixel(dc, 0, 0, RGB(255, 0, 0));
	SetPixel(dc, 1, 0, RGB(0, 255, 0));
	CHECK(bits[0] == 0x00 && bits[1] == 0xf8);
	CHECK(bits[2] == 0xe0 && bits[3] == 0x07);
	CHECK_EQ(GetObjectA(bm, sizeof(ds), &ds), sizeof(ds));
	CHECK_EQ(ds.dsBmih.biCompression, BI_BITFIELDS);
	CHECK_EQ(ds.dsBitfields[1], 0x07e0);
	SelectObject(dc, def);
	DeleteObject(bm);

	/* Bad formats. */
	header(&bi, 2, 2, 12, BI_RGB);
	CHECK(CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS,
			       (void **)&bits, NULL, 0) == NULL);
	CHECK(bits == NULL);
	header(&bi, 0, 2, 24, BI_RGB);
	CHECK(CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, NULL,
			       NULL, 0) == NULL);

	DeleteDC(dc);
	DeleteDC(dc2);
}

/* A DIB section in a page-file-backed mapping, at an offset. */
static void test_dib_section_mapping(void)
{
	FN(HBITMAP, CreateDIBSection, HDC, const BITMAPINFO *, UINT, void **,
	   HANDLE, DWORD);
	FN(BOOL, DeleteObject, HGDIOBJ);
	K32(HANDLE, CreateFileMappingA, HANDLE, LPSECURITY_ATTRIBUTES, DWORD,
	    DWORD, DWORD, LPCSTR);
	K32(LPVOID, MapViewOfFile, HANDLE, DWORD, DWORD, DWORD, SIZE_T);
	K32(BOOL, UnmapViewOfFile, LPCVOID);
	K32(BOOL, CloseHandle, HANDLE);

	HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
					0, 0x20000, NULL);
	CHECK(map != NULL);
	if (!map)
		return;
	struct bmi bi;
	BYTE *bits;
	header(&bi, 4, 4, 32, BI_RGB);
	HBITMAP bm = CreateDIBSection(NULL, (BITMAPINFO *)&bi, DIB_RGB_COLORS,
				      (void **)&bits, map, 0x10010);
	CHECK(bm != NULL);
	BYTE *view = MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
	CHECK(view != NULL);
	if (bm && view) {
		bits[0] = 0x5a;
		CHECK_EQ(view[0x10010], 0x5a);
	}
	UnmapViewOfFile(view);
	DeleteObject(bm);
	CloseHandle(map);
}

static void test_dibits(void)
{
	FN(HDC, CreateCompatibleDC, HDC);
	FN(BOOL, DeleteDC, HDC);
	FN(HBITMAP, CreateDIBSection, HDC, const BITMAPINFO *, UINT, void **,
	   HANDLE, DWORD);
	FN(HBITMAP, CreateBitmap, int, int, UINT, UINT, LPCVOID);
	FN(HBITMAP, CreateDIBitmap, HDC, const BITMAPINFOHEADER *, DWORD,
	   LPCVOID, const BITMAPINFO *, UINT);
	FN(int, GetDIBits, HDC, HBITMAP, UINT, UINT, LPVOID, LPBITMAPINFO, UINT);
	FN(int, SetDIBits, HDC, HBITMAP, UINT, UINT, LPCVOID, const BITMAPINFO *,
	   UINT);
	FN(LONG, GetBitmapBits, HBITMAP, LONG, LPVOID);
	FN(LONG, SetBitmapBits, HBITMAP, DWORD, LPCVOID);
	FN(BOOL, DeleteObject, HGDIOBJ);
	FN(int, GetObjectA, HGDIOBJ, int, LPVOID);

	HDC dc = CreateCompatibleDC(NULL);
	struct bmi bi;

	/* An 8-bit bottom-up DIB with its own palette into a 32-bit DDB. */
	header(&bi, 3, 2, 8, BI_RGB);
	bi.h.biClrUsed = 2;
	bi.colors[0] = (RGBQUAD){ 0x10, 0x20, 0x30, 0 };
	bi.colors[1] = (RGBQUAD){ 0xa0, 0xb0, 0xc0, 0 };
	static const BYTE src8[8] = { 0, 1, 0, 0, 1, 1, 0, 0 };	/* bottom row first */
	HBITMAP ddb = CreateDIBitmap(dc, &bi.h, CBM_INIT, src8, (BITMAPINFO *)&bi,
				     DIB_RGB_COLORS);
	CHECK(ddb != NULL);
	BITMAP b;
	GetObjectA(ddb, sizeof(b), &b);
	CHECK_EQ(b.bmBitsPixel, 1);	/* a fresh memory DC is monochrome */
	DeleteObject(ddb);
	ddb = CreateBitmap(3, 2, 1, 32, NULL);
	CHECK_EQ(SetDIBits(dc, ddb, 0, 2, src8, (BITMAPINFO *)&bi, DIB_RGB_COLORS), 2);

	/* The DDB layout: top-down, 32 bits. */
	DWORD px[6];
	CHECK_EQ(GetBitmapBits(ddb, sizeof(px), px), 24);
	CHECK_EQ(px[0], 0xc0b0a0);	/* top row = the DIB's second */
	CHECK_EQ(px[2], 0x302010);
	CHECK_EQ(px[4], 0xc0b0a0);
	px[5] = 0x123456;
	CHECK_EQ(SetBitmapBits(ddb, sizeof(px), px), 24);

	/* Describe, then read back as a top-down 24-bit DIB. */
	struct bmi out;
	memset(&out, 0, sizeof(out));
	out.h.biSize = sizeof(out.h);
	CHECK(GetDIBits(dc, ddb, 0, 2, NULL, (BITMAPINFO *)&out, DIB_RGB_COLORS));
	CHECK_EQ(out.h.biWidth, 3);
	CHECK_EQ(out.h.biHeight, 2);
	CHECK_EQ(out.h.biBitCount, 32);
	CHECK_EQ(out.h.biSizeImage, 24);
	header(&out, 3, -2, 24, BI_RGB);
	BYTE rgb[2 * 12];
	CHECK_EQ(GetDIBits(dc, ddb, 0, 2, rgb, (BITMAPINFO *)&out, DIB_RGB_COLORS), 2);
	CHECK_EQ(out.h.biSizeImage, 24);
	CHECK(rgb[0] == 0xa0 && rgb[1] == 0xb0 && rgb[2] == 0xc0);
	CHECK(rgb[12 + 6] == 0x56 && rgb[12 + 7] == 0x34 && rgb[12 + 8] == 0x12);

	/* Just the bottom scanline, bottom-up. */
	header(&out, 3, 2, 24, BI_RGB);
	memset(rgb, 0xee, sizeof(rgb));
	CHECK_EQ(GetDIBits(dc, ddb, 0, 1, rgb, (BITMAPINFO *)&out, DIB_RGB_COLORS), 1);
	CHECK(rgb[6] == 0x56 && rgb[12] == 0xee);

	/* To 8 bits: the halftone palette, nearest colours. */
	header(&out, 3, -2, 8, BI_RGB);
	BYTE idx[8];
	CHECK_EQ(GetDIBits(dc, ddb, 0, 2, idx, (BITMAPINFO *)&out, DIB_RGB_COLORS), 2);
	CHECK(out.colors[0].rgbRed == 0 && out.colors[255].rgbRed == 255);
	RGBQUAD c = out.colors[idx[0]];
	CHECK(abs(c.rgbRed - 0xc0) <= 26 && abs(c.rgbBlue - 0xa0) <= 26);

	/* A 32-bit DIB section read as 16 bits with 5-6-5 masks. */
	header(&bi, 1, 1, 32, BI_RGB);
	DWORD *bits32;
	HBITMAP sec = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS,
				       (void **)&bits32, NULL, 0);
	*bits32 = 0xff0000;	/* red */
	header(&out, 1, 1, 16, BI_BITFIELDS);
	WORD w16[2];
	CHECK_EQ(GetDIBits(dc, sec, 0, 1, w16, (BITMAPINFO *)&out, DIB_RGB_COLORS), 1);
	DWORD *m = (DWORD *)out.colors;
	CHECK_EQ(m[0], 0x7c00);
	CHECK_EQ(w16[0], 0x7c00);

	DeleteObject(sec);
	DeleteObject(ddb);
	DeleteDC(dc);
}

static void test_blits(void)
{
	FN(HDC, CreateCompatibleDC, HDC);
	FN(HDC, CreateDCA, LPCSTR, LPCSTR, LPCSTR, const void *);
	FN(BOOL, DeleteDC, HDC);
	FN(HBITMAP, CreateDIBSection, HDC, const BITMAPINFO *, UINT, void **,
	   HANDLE, DWORD);
	FN(HGDIOBJ, SelectObject, HDC, HGDIOBJ);
	FN(BOOL, DeleteObject, HGDIOBJ);
	FN(BOOL, BitBlt, HDC, int, int, int, int, HDC, int, int, DWORD);
	FN(BOOL, StretchBlt, HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
	FN(BOOL, PatBlt, HDC, int, int, int, int, DWORD);
	FN(int, StretchDIBits, HDC, int, int, int, int, int, int, int, int,
	   LPCVOID, const BITMAPINFO *, UINT, DWORD);
	FN(int, SetDIBitsToDevice, HDC, int, int, DWORD, DWORD, int, int, UINT,
	   UINT, LPCVOID, const BITMAPINFO *, UINT);
	FN(COLORREF, GetPixel, HDC, int, int);
	FN(HBRUSH, CreateSolidBrush, COLORREF);

	struct bmi bi;
	DWORD *src, *dst;
	HDC sdc = CreateCompatibleDC(NULL), ddc = CreateCompatibleDC(NULL);
	header(&bi, 2, -2, 32, BI_RGB);
	HBITMAP sbm = CreateDIBSection(NULL, (BITMAPINFO *)&bi, DIB_RGB_COLORS,
				       (void **)&src, NULL, 0);
	header(&bi, 4, -4, 32, BI_RGB);
	HBITMAP dbm = CreateDIBSection(NULL, (BITMAPINFO *)&bi, DIB_RGB_COLORS,
				       (void **)&dst, NULL, 0);
	HGDIOBJ sdef = SelectObject(sdc, sbm), ddef = SelectObject(ddc, dbm);
	src[0] = 0x0000ff;	/* blue (BGRA in memory, RGB in the DWORD) */
	src[1] = 0x00ff00;
	src[2] = 0xff0000;
	src[3] = 0xffffff;

	/* A plain copy, clipped at the edge. */
	CHECK(BitBlt(ddc, 3, 3, 2, 2, sdc, 0, 0, SRCCOPY));
	CHECK_EQ(dst[15], 0x0000ff);
	CHECK_EQ(dst[14], 0);

	/* Stretched 2x, then mirrored horizontally. */
	memset(dst, 0, 64);
	CHECK(StretchBlt(ddc, 0, 0, 4, 4, sdc, 0, 0, 2, 2, SRCCOPY));
	CHECK_EQ(dst[0], 0x0000ff);
	CHECK_EQ(dst[1], 0x0000ff);
	CHECK_EQ(dst[2], 0x00ff00);
	CHECK_EQ(dst[15], 0xffffff);
	CHECK(StretchBlt(ddc, 1, 0, -2, 1, sdc, 0, 0, 2, 1, SRCCOPY));
	CHECK_EQ(GetPixel(ddc, 0, 0), RGB(0, 255, 0));
	CHECK_EQ(GetPixel(ddc, 1, 0), RGB(0, 0, 255));

	/* Raster operations and brushes. */
	CHECK(PatBlt(ddc, 0, 0, 4, 4, BLACKNESS));
	CHECK_EQ(dst[5], 0);
	CHECK(PatBlt(ddc, 0, 0, 1, 1, DSTINVERT));
	CHECK_EQ(dst[0], 0xffffff);
	HBRUSH br = CreateSolidBrush(RGB(0x11, 0x22, 0x33));
	HGDIOBJ oldbr = SelectObject(ddc, br);
	CHECK(PatBlt(ddc, 1, 1, 2, 1, PATCOPY));
	CHECK_EQ(dst[5], 0x112233);
	CHECK_EQ(dst[6], 0x112233);
	CHECK_EQ(dst[7], 0);
	CHECK(BitBlt(ddc, 1, 1, 1, 1, sdc, 0, 0, SRCPAINT));
	CHECK_EQ(dst[5], 0x1122ff);
	CHECK(!BitBlt(ddc, 0, 0, 1, 1, sdc, 0, 0, 0x00aa0029));
	SelectObject(ddc, oldbr);
	DeleteObject(br);

	/* To the display: accepted and discarded. */
	HDC disp = CreateDCA("DISPLAY", NULL, NULL, NULL);
	CHECK(BitBlt(disp, 0, 0, 2, 2, sdc, 0, 0, SRCCOPY));
	CHECK_EQ(GetPixel(disp, 0, 0), 0);
	DeleteDC(disp);

	/* A 24-bit bottom-up DIB, from its lower-left origin. */
	header(&bi, 2, 2, 24, BI_RGB);
	static const BYTE dib24[16] = {
		1, 2, 3, 4, 5, 6, 0, 0,		/* bottom row */
		7, 8, 9, 10, 11, 12, 0, 0,	/* top row */
	};
	memset(dst, 0, 64);
	CHECK_EQ(StretchDIBits(ddc, 0, 0, 2, 2, 0, 0, 2, 2, dib24,
			       (BITMAPINFO *)&bi, DIB_RGB_COLORS, SRCCOPY), 2);
	CHECK_EQ(dst[0], 0x090807);
	CHECK_EQ(dst[4], 0x030201);
	/* The top row alone: ySrc = 1 from the bottom. */
	memset(dst, 0, 64);
	CHECK_EQ(StretchDIBits(ddc, 0, 0, 2, 1, 0, 1, 2, 1, dib24,
			       (BITMAPINFO *)&bi, DIB_RGB_COLORS, SRCCOPY), 1);
	CHECK_EQ(dst[0], 0x090807);
	CHECK_EQ(dst[4], 0);

	/* SetDIBitsToDevice with just the top scanline passed (a band). */
	memset(dst, 0, 64);
	CHECK_EQ(SetDIBitsToDevice(ddc, 0, 0, 2, 2, 0, 0, 1, 1, dib24 + 8,
				   (BITMAPINFO *)&bi, DIB_RGB_COLORS), 1);
	CHECK_EQ(dst[0], 0x090807);
	CHECK_EQ(dst[1], 0x0c0b0a);
	CHECK_EQ(dst[4], 0);

	/* Overlapping copy within one bitmap. */
	for (int i = 0; i < 16; i++)
		dst[i] = i;
	CHECK(BitBlt(ddc, 1, 0, 3, 1, ddc, 0, 0, SRCCOPY));
	CHECK(dst[1] == 0 && dst[2] == 1 && dst[3] == 2);

	SelectObject(sdc, sdef);
	SelectObject(ddc, ddef);
	DeleteObject(sbm);
	DeleteObject(dbm);
	DeleteDC(sdc);
	DeleteDC(ddc);
}

static void test_palettes(void)
{
	FN(HPALETTE, CreatePalette, const LOGPALETTE *);
	FN(HPALETTE, CreateHalftonePalette, HDC);
	FN(UINT, GetPaletteEntries, HPALETTE, UINT, UINT, LPPALETTEENTRY);
	FN(UINT, SetPaletteEntries, HPALETTE, UINT, UINT, const PALETTEENTRY *);
	FN(UINT, GetNearestPaletteIndex, HPALETTE, COLORREF);
	FN(HPALETTE, SelectPalette, HDC, HPALETTE, BOOL);
	FN(UINT, RealizePalette, HDC);
	FN(HDC, CreateCompatibleDC, HDC);
	FN(BOOL, DeleteDC, HDC);
	FN(BOOL, DeleteObject, HGDIOBJ);
	FN(HGDIOBJ, GetStockObject, int);
	FN(int, GetObjectA, HGDIOBJ, int, LPVOID);
	FN(HBITMAP, CreateDIBSection, HDC, const BITMAPINFO *, UINT, void **,
	   HANDLE, DWORD);
	FN(int, GetDIBColorTable, HDC, UINT, UINT, RGBQUAD *);
	FN(HGDIOBJ, SelectObject, HDC, HGDIOBJ);

	struct {
		WORD version, n;
		PALETTEENTRY e[3];
	} lp = { 0x300, 3, { { 255, 0, 0, 0 }, { 0, 255, 0, 0 }, { 0, 0, 255, 0 } } };
	HPALETTE pal = CreatePalette((LOGPALETTE *)&lp);
	CHECK(pal != NULL);
	WORD n = 0;
	CHECK_EQ(GetObjectA(pal, sizeof(n), &n), 2);
	CHECK_EQ(n, 3);
	CHECK_EQ(GetPaletteEntries(pal, 0, 0, NULL), 3);
	PALETTEENTRY e[4];
	CHECK_EQ(GetPaletteEntries(pal, 1, 4, e), 2);
	CHECK_EQ(e[0].peGreen, 255);
	CHECK_EQ(GetNearestPaletteIndex(pal, RGB(10, 10, 200)), 2);
	PALETTEENTRY grey = { 128, 128, 128, 0 };
	CHECK_EQ(SetPaletteEntries(pal, 0, 1, &grey), 1);
	CHECK_EQ(GetNearestPaletteIndex(pal, RGB(120, 120, 120)), 0);

	/* DIB_PAL_COLORS: the table indexes the DC's palette. */
	HDC dc = CreateCompatibleDC(NULL);
	CHECK(SelectPalette(dc, pal, FALSE) == GetStockObject(DEFAULT_PALETTE));
	CHECK_EQ(RealizePalette(dc), 0);
	struct {
		BITMAPINFOHEADER h;
		WORD idx[2];
	} bi = { { sizeof(BITMAPINFOHEADER), 1, 1, 1, 1, BI_RGB, 0, 0, 0, 2, 0 },
		 { 2, 1 } };
	HBITMAP bm = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_PAL_COLORS,
				      NULL, NULL, 0);
	CHECK(bm != NULL);
	HGDIOBJ def = SelectObject(dc, bm);
	RGBQUAD q[2];
	CHECK_EQ(GetDIBColorTable(dc, 0, 2, q), 2);
	CHECK(q[0].rgbBlue == 255 && q[1].rgbGreen == 255);
	SelectObject(dc, def);
	DeleteObject(bm);

	CHECK(!DeleteObject(pal));	/* still selected */
	SelectPalette(dc, GetStockObject(DEFAULT_PALETTE), FALSE);
	CHECK(DeleteObject(pal));

	HPALETTE half = CreateHalftonePalette(dc);
	CHECK_EQ(GetPaletteEntries(half, 0, 0, NULL), 256);
	DeleteObject(half);
	CHECK_EQ(GetPaletteEntries(GetStockObject(DEFAULT_PALETTE), 0, 0, NULL), 20);
	DeleteDC(dc);
}

int main(void)
{
	CHECK_EQ(lowmem_reserve(), 0);
	CHECK_EQ(runtime_enter(), 0);
	gdi = mod_load("gdi32.dll", NULL);
	k32 = mod_load("kernel32.dll", NULL);
	CHECK(gdi && k32);
	if (!gdi || !k32)
		return test_summary("test_gdi32");
	CHECK(gdi->builtin && !gdi->builtin->fallback);

	test_objects();
	test_dc_state();
	test_dib_section();
	test_dib_section_mapping();
	test_dibits();
	test_blits();
	test_palettes();

	mod_free(gdi);
	mod_free(k32);
	return test_summary("test_gdi32");
}
