/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Checks the include/win32 headers against known Windows values. This runs
 * without mingw-w64; tests/abi/ does a fuller cross-check when it is present.
 */
#include <string.h>

#include "win32.h"
#include "test.h"

/* Sizes a 32-bit Windows build gives these types. */
_Static_assert(sizeof(WCHAR) == 2, "WCHAR is UTF-16");
_Static_assert(sizeof(LONG) == 4, "LONG is 32-bit");
_Static_assert(sizeof(LONGLONG) == 8, "LONGLONG is 64-bit");
_Static_assert(sizeof(void *) == 4, "32-bit pointers");
_Static_assert(sizeof(BITMAPINFOHEADER) == 40, "BITMAPINFOHEADER");
_Static_assert(sizeof(BITMAPFILEHEADER) == 14, "BITMAPFILEHEADER");
_Static_assert(sizeof(ICOPEN) == 36, "ICOPEN");
_Static_assert(sizeof(ICINFO) == 568, "ICINFO");
_Static_assert(sizeof(ICCOMPRESS) == 48, "ICCOMPRESS");
_Static_assert(sizeof(ICDECOMPRESS) == 24, "ICDECOMPRESS");
_Static_assert(sizeof(ICDECOMPRESSEX) == 52, "ICDECOMPRESSEX");
_Static_assert(sizeof(ICCOMPRESSFRAMES) == 64, "ICCOMPRESSFRAMES");
_Static_assert(sizeof(AM_MEDIA_TYPE) == 72, "AM_MEDIA_TYPE");
_Static_assert(sizeof(VIDEOINFOHEADER) == 88, "VIDEOINFOHEADER");
_Static_assert(offsetof(VIDEOINFOHEADER, AvgTimePerFrame) == 40,
	       "VIDEOINFOHEADER.AvgTimePerFrame");
_Static_assert(sizeof(VIDEOINFOHEADER2) == 112, "VIDEOINFOHEADER2");
_Static_assert(sizeof(AM_SAMPLE2_PROPERTIES) == 48, "AM_SAMPLE2_PROPERTIES");
_Static_assert(sizeof(PIN_INFO) == 264, "PIN_INFO");
_Static_assert(sizeof(STATSTG) == 72, "STATSTG");

/* 64-bit members are 8-byte aligned, as on Windows (not 4, as on Linux). */
struct align_probe {
	BYTE tag;
	LONGLONG value;
};
_Static_assert(offsetof(struct align_probe, value) == 8, "LONGLONG alignment");
_Static_assert(sizeof(struct align_probe) == 16, "LONGLONG struct padding");
struct union_probe {
	DWORD tag;
	LARGE_INTEGER value;
};
_Static_assert(offsetof(struct union_probe, value) == 8, "LARGE_INTEGER alignment");

int main(void)
{
	/* A FOURCC stores its characters in file order. */
	DWORD vidc = ICTYPE_VIDEO;
	CHECK(memcmp(&vidc, "vidc", 4) == 0);
	CHECK_EQ(mmioFOURCC('D', 'I', 'B', ' '), 0x20424944);

	/* Message numbers codecs switch on. */
	CHECK_EQ(ICM_GETINFO, 0x5002);
	CHECK_EQ(ICM_COMPRESS, 0x4008);
	CHECK_EQ(ICM_DECOMPRESS, 0x400D);
	CHECK_EQ(ICM_DECOMPRESSEX, 0x403E);

	/* Error codes are negative LRESULTs. */
	LRESULT err = ICERR_UNSUPPORTED;
	CHECK(err < 0);
	CHECK_EQ(ICERR_BADFORMAT, -2);

	/* Word helpers. */
	CHECK_EQ(LOWORD(0x12345678), 0x5678);
	CHECK_EQ(HIWORD(0x12345678), 0x1234);
	CHECK_EQ((DWORD)MAKELONG(0x5678, 0x1234), 0x12345678);
	CHECK_EQ(MAKEWORD(0x34, 0x12), 0x1234);

	/* FOURCC subtypes put the FOURCC in Data1 of the 'vids' shape. */
	static const GUID yuy2 = FOURCC_SUBTYPE_INIT(mmioFOURCC('Y', 'U', 'Y', '2'));
	CHECK(memcmp(&yuy2, &MEDIASUBTYPE_YUY2, sizeof(GUID)) == 0);
	static const GUID vids = FOURCC_SUBTYPE_INIT(mmioFOURCC('v', 'i', 'd', 's'));
	CHECK(memcmp(&vids, &MEDIATYPE_Video, sizeof(GUID)) == 0);
	CHECK_EQ(IID_IPin.Data1, 0x56a86891);
	CHECK_EQ(IID_IUnknown.Data4[0], 0xc0);

	return test_summary("test_win32_types");
}
