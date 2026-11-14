/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gdiplus.dll, the flat API, with no drawing behind it. GdiplusStartup
 * succeeds and GdipAlloc/GdipFree work, so a codec that starts GDI+ for its
 * property pages or a thumbnail (fiyuv, mvcVfwRefAVI, wmvdspa) loads and
 * runs; every call that would make or draw an image says NotImplemented
 * (InvalidParameter given no object) and returns no object, so nothing
 * that depends on one ever runs. No encoders or decoders are listed.
 *
 * The functions take their Windows argument counts, which matter: they are
 * stdcall, and on i386 the callee pops its arguments. Only the imports seen
 * in codecs are here; any other traps.
 */
#include <stdint.h>
#include <stdlib.h>

#include "win32/internal.h"

typedef int GpStatus;

enum { Ok = 0, GenericError = 1, InvalidParameter = 2, NotImplemented = 6 };

static GpStatus unavailable(uintptr_t object)
{
	return object ? NotImplemented : InvalidParameter;
}

typedef struct {
	UINT GdiplusVersion;
	void *DebugEventCallback;
	BOOL SuppressBackgroundThread;
	BOOL SuppressExternalCodecs;
} GdiplusStartupInput;

typedef struct {
	void *NotificationHook;
	void *NotificationUnhook;
} GdiplusStartupOutput;

WINFN(GpStatus) gp_NotificationHook(ULONG_PTR *token)
{
	if (!token)
		return InvalidParameter;
	*token = 1;
	return Ok;
}

WINFN(void) gp_NotificationUnhook(ULONG_PTR token)
{
}

WINFN(GpStatus) gp_GdiplusStartup(ULONG_PTR *token, const GdiplusStartupInput *in,
				  GdiplusStartupOutput *out)
{
	if (!token || !in || in->GdiplusVersion < 1 || in->GdiplusVersion > 3)
		return InvalidParameter;
	*token = 1;
	if (out) {
		/* The background thread's work, should the caller run it. */
		out->NotificationHook = (void *)gp_NotificationHook;
		out->NotificationUnhook = (void *)gp_NotificationUnhook;
	}
	return Ok;
}

WINFN(void) gp_GdiplusShutdown(ULONG_PTR token)
{
}

WINFN(void *) gp_GdipAlloc(size_t size)
{
	return malloc(size ? size : 1);
}

WINFN(void) gp_GdipFree(void *p)
{
	free(p);
}

static GpStatus no_codecs(UINT *num, UINT *size)
{
	if (!num || !size)
		return InvalidParameter;
	*num = 0;
	*size = 0;
	return Ok;
}

WINFN(GpStatus) gp_GdipGetImageEncodersSize(UINT *num, UINT *size) { return no_codecs(num, size); }
WINFN(GpStatus) gp_GdipGetImageDecodersSize(UINT *num, UINT *size) { return no_codecs(num, size); }

WINFN(GpStatus) gp_GdipGetImageDecoders(UINT num, UINT size, void *codecs)
{
	return codecs ? (num || size ? InvalidParameter : Ok) : InvalidParameter;
}

/* Everything else: no object can exist to work on. */
WINFN(GpStatus) gp_GdipAddPathArcI(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipBitmapLockBits(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipBitmapUnlockBits(uintptr_t a0, uintptr_t a1)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipCloneBrush(uintptr_t a0, uintptr_t a1)
{
	if (a1)
		*(void **)a1 = NULL;
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipCloneImage(uintptr_t a0, uintptr_t a1)
{
	if (a1)
		*(void **)a1 = NULL;
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipCreateBitmapFromFile(uintptr_t a0, uintptr_t a1)
{
	if (a1)
		*(void **)a1 = NULL;
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipCreateBitmapFromFileICM(uintptr_t a0, uintptr_t a1)
{
	if (a1)
		*(void **)a1 = NULL;
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipCreateBitmapFromGdiDib(uintptr_t a0, uintptr_t a1, uintptr_t a2)
{
	if (a2)
		*(void **)a2 = NULL;
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipCreateBitmapFromGraphics(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
	if (a3)
		*(void **)a3 = NULL;
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipCreateBitmapFromHBITMAP(uintptr_t a0, uintptr_t a1, uintptr_t a2)
{
	if (a2)
		*(void **)a2 = NULL;
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipCreateBitmapFromScan0(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
	if (a5)
		*(void **)a5 = NULL;
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipCreateBitmapFromStream(uintptr_t a0, uintptr_t a1)
{
	if (a1)
		*(void **)a1 = NULL;
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipCreateBitmapFromStreamICM(uintptr_t a0, uintptr_t a1)
{
	if (a1)
		*(void **)a1 = NULL;
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipCreateFromHDC(uintptr_t a0, uintptr_t a1)
{
	if (a1)
		*(void **)a1 = NULL;
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipCreateFromHWND(uintptr_t a0, uintptr_t a1)
{
	if (a1)
		*(void **)a1 = NULL;
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipCreateHBITMAPFromBitmap(uintptr_t a0, uintptr_t a1, uintptr_t a2)
{
	if (a1)
		*(void **)a1 = NULL;
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipCreatePath(uintptr_t a0, uintptr_t a1)
{
	if (a1)
		*(void **)a1 = NULL;
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipCreatePen1(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
	if (a3)
		*(void **)a3 = NULL;
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipCreateSolidFill(uintptr_t a0, uintptr_t a1)
{
	if (a1)
		*(void **)a1 = NULL;
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipDeleteBrush(uintptr_t a0)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipDeleteGraphics(uintptr_t a0)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipDeletePath(uintptr_t a0)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipDeletePen(uintptr_t a0)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipDisposeImage(uintptr_t a0)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipDrawArcI(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipDrawEllipseI(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipDrawImageI(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipDrawImageRect(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipDrawImageRectI(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipDrawImageRectRectI(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12, uintptr_t a13)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipDrawLineI(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipDrawPieI(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipDrawRectangleI(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipFillEllipseI(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipFillPieI(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipFillRectangleI(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipGetImageEncoders(uintptr_t a0, uintptr_t a1, uintptr_t a2)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipGetImageGraphicsContext(uintptr_t a0, uintptr_t a1)
{
	if (a1)
		*(void **)a1 = NULL;
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipGetImageHeight(uintptr_t a0, uintptr_t a1)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipGetImagePalette(uintptr_t a0, uintptr_t a1, uintptr_t a2)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipGetImagePaletteSize(uintptr_t a0, uintptr_t a1)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipGetImagePixelFormat(uintptr_t a0, uintptr_t a1)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipGetImageThumbnail(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
	if (a3)
		*(void **)a3 = NULL;
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipGetImageWidth(uintptr_t a0, uintptr_t a1)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipGetPathLastPoint(uintptr_t a0, uintptr_t a1)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipImageRotateFlip(uintptr_t a0, uintptr_t a1)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipLoadImageFromFile(uintptr_t a0, uintptr_t a1)
{
	if (a1)
		*(void **)a1 = NULL;
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipLoadImageFromStream(uintptr_t a0, uintptr_t a1)
{
	if (a1)
		*(void **)a1 = NULL;
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipSaveImageToFile(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipSaveImageToStream(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipSetClipRectI(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipSetCompositingMode(uintptr_t a0, uintptr_t a1)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipSetInterpolationMode(uintptr_t a0, uintptr_t a1)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipSetSmoothingMode(uintptr_t a0, uintptr_t a1)
{
	return unavailable(a0);
}

WINFN(GpStatus) gp_GdipSetSolidFillColor(uintptr_t a0, uintptr_t a1)
{
	return unavailable(a0);
}

#define E(name) { #name, 0, (void *)gp_##name }

static const struct builtin_export exports[] = {
	E(GdipAddPathArcI),
	E(GdipAlloc),
	E(GdipBitmapLockBits),
	E(GdipBitmapUnlockBits),
	E(GdipCloneBrush),
	E(GdipCloneImage),
	E(GdipCreateBitmapFromFile),
	E(GdipCreateBitmapFromFileICM),
	E(GdipCreateBitmapFromGdiDib),
	E(GdipCreateBitmapFromGraphics),
	E(GdipCreateBitmapFromHBITMAP),
	E(GdipCreateBitmapFromScan0),
	E(GdipCreateBitmapFromStream),
	E(GdipCreateBitmapFromStreamICM),
	E(GdipCreateFromHDC),
	E(GdipCreateFromHWND),
	E(GdipCreateHBITMAPFromBitmap),
	E(GdipCreatePath),
	E(GdipCreatePen1),
	E(GdipCreateSolidFill),
	E(GdipDeleteBrush),
	E(GdipDeleteGraphics),
	E(GdipDeletePath),
	E(GdipDeletePen),
	E(GdipDisposeImage),
	E(GdipDrawArcI),
	E(GdipDrawEllipseI),
	E(GdipDrawImageI),
	E(GdipDrawImageRect),
	E(GdipDrawImageRectI),
	E(GdipDrawImageRectRectI),
	E(GdipDrawLineI),
	E(GdipDrawPieI),
	E(GdipDrawRectangleI),
	E(GdipFillEllipseI),
	E(GdipFillPieI),
	E(GdipFillRectangleI),
	E(GdipFree),
	E(GdipGetImageDecoders),
	E(GdipGetImageDecodersSize),
	E(GdipGetImageEncoders),
	E(GdipGetImageEncodersSize),
	E(GdipGetImageGraphicsContext),
	E(GdipGetImageHeight),
	E(GdipGetImagePalette),
	E(GdipGetImagePaletteSize),
	E(GdipGetImagePixelFormat),
	E(GdipGetImageThumbnail),
	E(GdipGetImageWidth),
	E(GdipGetPathLastPoint),
	E(GdipImageRotateFlip),
	E(GdipLoadImageFromFile),
	E(GdipLoadImageFromStream),
	E(GdipSaveImageToFile),
	E(GdipSaveImageToStream),
	E(GdipSetClipRectI),
	E(GdipSetCompositingMode),
	E(GdipSetInterpolationMode),
	E(GdipSetSmoothingMode),
	E(GdipSetSolidFillColor),
	E(GdiplusShutdown),
	E(GdiplusStartup),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_gdiplus = { "gdiplus.dll", NULL, tables, 0 };
