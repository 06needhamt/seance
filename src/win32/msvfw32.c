/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * msvfw32.dll: the ICM API from src/vfw/icm.c, for codecs that call back
 * into it (wrappers opening another codec, for instance), the
 * ICSeqCompressFrame* sequence API, ICImageCompress/ICImageDecompress, and
 * ICCompressorChoose answering without its dialog, and DrawDib* with
 * ICDrawBegin/ICDraw and StretchDIB (src/vfw/drawdib.c). MCIWnd* and the
 * rest of the GUI side are left as traps.
 */
#include "vfw/icm.h"
#include "win32/internal.h"

/* 4.03.950, what Wine reports. */
WINFN(DWORD) vfw_VideoForWindowsVersion(void)
{
	return 0x040003B6;
}

static const struct builtin_export exports[] = {
	{ "ICClose", 0, (void *)ic_close },
	{ "ICCompress", 0, (void *)ic_compress },
	{ "ICCompressorChoose", 0, (void *)ic_compressor_choose },
	{ "ICCompressorFree", 0, (void *)ic_compressor_free },
	{ "ICDecompress", 0, (void *)ic_decompress },
	{ "ICGetDisplayFormat", 0, (void *)ic_get_display_format },
	{ "ICGetInfo", 0, (void *)ic_get_info },
	{ "ICImageCompress", 0, (void *)ic_image_compress },
	{ "ICImageDecompress", 0, (void *)ic_image_decompress },
	{ "ICInfo", 0, (void *)ic_info },
	{ "ICInstall", 0, (void *)ic_install },
	{ "ICLocate", 0, (void *)ic_locate },
	{ "ICOpen", 0, (void *)ic_open },
	{ "ICOpenFunction", 0, (void *)ic_open_function },
	{ "ICRemove", 0, (void *)ic_remove },
	{ "ICSendMessage", 0, (void *)ic_send },
	{ "ICSeqCompressFrame", 0, (void *)ic_seq_frame },
	{ "ICSeqCompressFrameEnd", 0, (void *)ic_seq_end },
	{ "ICSeqCompressFrameStart", 0, (void *)ic_seq_start },
	{ "VideoForWindowsVersion", 0, (void *)vfw_VideoForWindowsVersion },
	BUILTIN_END
};

extern const struct builtin_export vfw_drawdib_exports[];

static const struct builtin_export *const tables[] = {
	exports, vfw_drawdib_exports, NULL
};

const struct builtin_dll builtin_msvfw32 = { "msvfw32.dll", NULL, tables, 0 };
