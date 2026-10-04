/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * GDI bitmap and palette definitions used by VfW codecs.
 */
#ifndef VFW_WIN32_WINGDI_H
#define VFW_WIN32_WINGDI_H

#include "types.h"

/* biCompression values. */
#define BI_RGB       0
#define BI_RLE8      1
#define BI_RLE4      2
#define BI_BITFIELDS 3
#define BI_JPEG      4
#define BI_PNG       5

typedef struct tagRGBQUAD {
	BYTE rgbBlue;
	BYTE rgbGreen;
	BYTE rgbRed;
	BYTE rgbReserved;
} RGBQUAD, *LPRGBQUAD;

typedef struct tagPALETTEENTRY {
	BYTE peRed;
	BYTE peGreen;
	BYTE peBlue;
	BYTE peFlags;
} PALETTEENTRY, *PPALETTEENTRY, *LPPALETTEENTRY;

typedef struct tagBITMAPINFOHEADER {
	DWORD biSize;
	LONG biWidth;
	LONG biHeight;
	WORD biPlanes;
	WORD biBitCount;
	DWORD biCompression;
	DWORD biSizeImage;
	LONG biXPelsPerMeter;
	LONG biYPelsPerMeter;
	DWORD biClrUsed;
	DWORD biClrImportant;
} BITMAPINFOHEADER, *PBITMAPINFOHEADER, *LPBITMAPINFOHEADER;

typedef struct tagBITMAPINFO {
	BITMAPINFOHEADER bmiHeader;
	RGBQUAD bmiColors[1];
} BITMAPINFO, *PBITMAPINFO, *LPBITMAPINFO;

typedef struct tagBITMAP {
	LONG bmType;
	LONG bmWidth;
	LONG bmHeight;
	LONG bmWidthBytes;
	WORD bmPlanes;
	WORD bmBitsPixel;
	LPVOID bmBits;
} BITMAP, *PBITMAP, *LPBITMAP;

typedef struct tagDIBSECTION {
	BITMAP dsBm;
	BITMAPINFOHEADER dsBmih;
	DWORD dsBitfields[3];
	HANDLE dshSection;
	DWORD dsOffset;
} DIBSECTION, *PDIBSECTION, *LPDIBSECTION;

typedef struct tagLOGPALETTE {
	WORD palVersion;
	WORD palNumEntries;
	PALETTEENTRY palPalEntry[1];
} LOGPALETTE, *PLOGPALETTE, *LPLOGPALETTE;

typedef struct tagLOGBRUSH {
	UINT lbStyle;
	COLORREF lbColor;
	ULONG_PTR lbHatch;
} LOGBRUSH, *PLOGBRUSH, *LPLOGBRUSH;

typedef struct tagLOGPEN {
	UINT lopnStyle;
	POINT lopnWidth;
	COLORREF lopnColor;
} LOGPEN, *PLOGPEN, *LPLOGPEN;

#define LF_FACESIZE 32

typedef struct tagLOGFONTA {
	LONG lfHeight;
	LONG lfWidth;
	LONG lfEscapement;
	LONG lfOrientation;
	LONG lfWeight;
	BYTE lfItalic;
	BYTE lfUnderline;
	BYTE lfStrikeOut;
	BYTE lfCharSet;
	BYTE lfOutPrecision;
	BYTE lfClipPrecision;
	BYTE lfQuality;
	BYTE lfPitchAndFamily;
	CHAR lfFaceName[LF_FACESIZE];
} LOGFONTA, *PLOGFONTA, *LPLOGFONTA;

typedef struct tagLOGFONTW {
	LONG lfHeight;
	LONG lfWidth;
	LONG lfEscapement;
	LONG lfOrientation;
	LONG lfWeight;
	BYTE lfItalic;
	BYTE lfUnderline;
	BYTE lfStrikeOut;
	BYTE lfCharSet;
	BYTE lfOutPrecision;
	BYTE lfClipPrecision;
	BYTE lfQuality;
	BYTE lfPitchAndFamily;
	WCHAR lfFaceName[LF_FACESIZE];
} LOGFONTW, *PLOGFONTW, *LPLOGFONTW;

typedef struct tagTEXTMETRICA {
	LONG tmHeight;
	LONG tmAscent;
	LONG tmDescent;
	LONG tmInternalLeading;
	LONG tmExternalLeading;
	LONG tmAveCharWidth;
	LONG tmMaxCharWidth;
	LONG tmWeight;
	LONG tmOverhang;
	LONG tmDigitizedAspectX;
	LONG tmDigitizedAspectY;
	BYTE tmFirstChar;
	BYTE tmLastChar;
	BYTE tmDefaultChar;
	BYTE tmBreakChar;
	BYTE tmItalic;
	BYTE tmUnderlined;
	BYTE tmStruckOut;
	BYTE tmPitchAndFamily;
	BYTE tmCharSet;
} TEXTMETRICA, *PTEXTMETRICA, *LPTEXTMETRICA;

typedef struct tagTEXTMETRICW {
	LONG tmHeight;
	LONG tmAscent;
	LONG tmDescent;
	LONG tmInternalLeading;
	LONG tmExternalLeading;
	LONG tmAveCharWidth;
	LONG tmMaxCharWidth;
	LONG tmWeight;
	LONG tmOverhang;
	LONG tmDigitizedAspectX;
	LONG tmDigitizedAspectY;
	WCHAR tmFirstChar;
	WCHAR tmLastChar;
	WCHAR tmDefaultChar;
	WCHAR tmBreakChar;
	BYTE tmItalic;
	BYTE tmUnderlined;
	BYTE tmStruckOut;
	BYTE tmPitchAndFamily;
	BYTE tmCharSet;
} TEXTMETRICW, *PTEXTMETRICW, *LPTEXTMETRICW;

#define RGB(r, g, b) \
	((COLORREF)(((BYTE)(r)) | ((WORD)((BYTE)(g)) << 8) | \
		    ((DWORD)((BYTE)(b)) << 16)))
#define GetRValue(c) ((BYTE)(c))
#define GetGValue(c) ((BYTE)((c) >> 8))
#define GetBValue(c) ((BYTE)((c) >> 16))

#define CLR_INVALID 0xffffffff
#define GDI_ERROR   0xffffffff

/* DIB colour tables and CreateDIBitmap. */
#define DIB_RGB_COLORS 0
#define DIB_PAL_COLORS 1
#define CBM_INIT       0x04

/* Raster operations. */
#define SRCCOPY     0x00cc0020
#define SRCPAINT    0x00ee0086
#define SRCAND      0x008800c6
#define SRCINVERT   0x00660046
#define SRCERASE    0x00440328
#define NOTSRCCOPY  0x00330008
#define NOTSRCERASE 0x001100a6
#define PATCOPY     0x00f00021
#define PATINVERT   0x005a0049
#define DSTINVERT   0x00550009
#define BLACKNESS   0x00000042
#define WHITENESS   0x00ff0062

/* GetObjectType. */
#define OBJ_PEN         1
#define OBJ_BRUSH       2
#define OBJ_DC          3
#define OBJ_METADC      4
#define OBJ_PAL         5
#define OBJ_FONT        6
#define OBJ_BITMAP      7
#define OBJ_REGION      8
#define OBJ_METAFILE    9
#define OBJ_MEMDC       10
#define OBJ_EXTPEN      11
#define OBJ_ENHMETADC   12
#define OBJ_ENHMETAFILE 13

#ifndef BS_PATTERN
#define BS_PATTERN 3
#endif

/* GetStockObject. */
#define WHITE_BRUSH         0
#define LTGRAY_BRUSH        1
#define GRAY_BRUSH          2
#define DKGRAY_BRUSH        3
#define BLACK_BRUSH         4
#define NULL_BRUSH          5
#define HOLLOW_BRUSH        5
#define WHITE_PEN           6
#define BLACK_PEN           7
#define NULL_PEN            8
#define OEM_FIXED_FONT      10
#define ANSI_FIXED_FONT     11
#define ANSI_VAR_FONT       12
#define SYSTEM_FONT         13
#define DEVICE_DEFAULT_FONT 14
#define DEFAULT_PALETTE     15
#define SYSTEM_FIXED_FONT   16
#define DEFAULT_GUI_FONT    17
#define DC_BRUSH            18
#define DC_PEN              19

/* GetDeviceCaps. */
#define DRIVERVERSION  0
#define TECHNOLOGY     2
#define HORZSIZE       4
#define VERTSIZE       6
#define HORZRES        8
#define VERTRES        10
#define BITSPIXEL      12
#define PLANES         14
#define NUMBRUSHES     16
#define NUMPENS        18
#define NUMFONTS       22
#define NUMCOLORS      24
#define CURVECAPS      28
#define LINECAPS       30
#define POLYGONALCAPS  32
#define TEXTCAPS       34
#define CLIPCAPS       36
#define RASTERCAPS     38
#define ASPECTX        40
#define ASPECTY        42
#define ASPECTXY       44
#define LOGPIXELSX     88
#define LOGPIXELSY     90
#define SIZEPALETTE    104
#define NUMRESERVED    106
#define COLORRES       108
#define VREFRESH       116
#define DESKTOPVERTRES 117
#define DESKTOPHORZRES 118
#define BLTALIGNMENT   119

#define DT_RASDISPLAY 1

#define RC_BITBLT      0x0001
#define RC_BITMAP64    0x0008
#define RC_DI_BITMAP   0x0080
#define RC_PALETTE     0x0100
#define RC_DIBTODEV    0x0200
#define RC_STRETCHBLT  0x0800
#define RC_STRETCHDIB  0x2000

/* Background modes, brushes and pens. */
#define TRANSPARENT 1
#define OPAQUE      2
#define BS_SOLID    0
#define BS_NULL     1
#define BS_HOLLOW   1
#define PS_SOLID    0
#define PS_NULL     5

/* Palettes. */
#define PC_RESERVED   0x01
#define PC_EXPLICIT   0x02
#define PC_NOCOLLAPSE 0x04

/* SetStretchBltMode. */
#define BLACKONWHITE        1
#define WHITEONBLACK        2
#define COLORONCOLOR        3
#define HALFTONE            4
#define STRETCH_ANDSCANS    1
#define STRETCH_ORSCANS     2
#define STRETCH_DELETESCANS 3
#define STRETCH_HALFTONE    4

/* Fonts. */
#define FW_DONTCARE    0
#define FW_NORMAL      400
#define FW_BOLD        700
#define ANSI_CHARSET    0
#define DEFAULT_CHARSET 1
#define OUT_DEFAULT_PRECIS  0
#define CLIP_DEFAULT_PRECIS 0
#define DEFAULT_QUALITY     0
#define VARIABLE_PITCH      2
#define FF_SWISS            0x20

/* BITMAPFILEHEADER is 2-byte packed on Windows (14 bytes). */
#pragma pack(push, 2)
typedef struct tagBITMAPFILEHEADER {
	WORD bfType;
	DWORD bfSize;
	WORD bfReserved1;
	WORD bfReserved2;
	DWORD bfOffBits;
} BITMAPFILEHEADER, *PBITMAPFILEHEADER, *LPBITMAPFILEHEADER;
#pragma pack(pop)

#endif /* VFW_WIN32_WINGDI_H */
