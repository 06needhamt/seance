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

	return test_summary("test_win32_types");
}
