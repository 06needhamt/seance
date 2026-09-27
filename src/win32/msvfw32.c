/*
 * msvfw32.dll: the ICM API from src/vfw/icm.c, for codecs that call back
 * into it (wrappers opening another codec, for instance). DrawDib*,
 * MCIWnd*, ICCompressorChoose and the rest of the GUI side are left as
 * traps.
 */
#include "vfw/icm.h"
#include "win32/internal.h"

/* 4.03.950, what Wine reports. */
WINFN(DWORD) vfw_VideoForWindowsVersion(void)
{
	return 0x040003B6;
}

/* Drawing is not supported: there is no display. */
static DWORD STUB_CDECL vfw_ICDrawBegin(void)
{
	return (DWORD)ICERR_UNSUPPORTED;
}

static DWORD STUB_CDECL vfw_ICDraw(void)
{
	return (DWORD)ICERR_UNSUPPORTED;
}

static const struct builtin_export exports[] = {
	{ "ICClose", 0, (void *)ic_close },
	{ "ICCompress", 0, (void *)ic_compress },
	{ "ICDecompress", 0, (void *)ic_decompress },
	{ "ICDraw", 0, (void *)vfw_ICDraw },
	{ "ICDrawBegin", 0, (void *)vfw_ICDrawBegin },
	{ "ICGetDisplayFormat", 0, (void *)ic_get_display_format },
	{ "ICGetInfo", 0, (void *)ic_get_info },
	{ "ICInfo", 0, (void *)ic_info },
	{ "ICInstall", 0, (void *)ic_install },
	{ "ICLocate", 0, (void *)ic_locate },
	{ "ICOpen", 0, (void *)ic_open },
	{ "ICOpenFunction", 0, (void *)ic_open_function },
	{ "ICRemove", 0, (void *)ic_remove },
	{ "ICSendMessage", 0, (void *)ic_send },
	{ "VideoForWindowsVersion", 0, (void *)vfw_VideoForWindowsVersion },
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_msvfw32 = { "msvfw32.dll", NULL, tables, 0 };
