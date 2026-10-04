/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * avifil32.dll.  Seance has its own AVI reader, but does not expose the
 * COM-based AVIFile interfaces to loaded Windows code.  Export the legacy
 * API so codecs which contain optional AVI utilities can still load, and
 * fail those utilities cleanly with AVIERR_UNSUPPORTED.
 */
#include "win32/internal.h"

WINFN(void) avifil_AVIFileInit(void) {}
WINFN(void) avifil_AVIFileExit(void) {}

static HRESULT unsupported(void **out)
{
	if (out)
		*out = NULL;
	return AVIERR_UNSUPPORTED;
}

WINFN(HRESULT) avifil_AVIFileOpenA(void **file, const char *path, UINT mode,
				   const GUID *handler)
{
	return unsupported(file);
}

WINFN(HRESULT) avifil_AVIFileOpenW(void **file, const WCHAR *path, UINT mode,
				   const GUID *handler)
{
	return unsupported(file);
}

WINFN(HRESULT) avifil_AVIFileGetStream(void *file, void **stream, DWORD type,
				       LONG index)
{
	return unsupported(stream);
}

WINFN(ULONG) avifil_AVIFileRelease(void *file) { return 0; }
WINFN(ULONG) avifil_AVIStreamRelease(void *stream) { return 0; }

WINFN(LONG) avifil_AVIStreamLength(void *stream) { return -1; }

WINFN(LONG) avifil_AVIStreamSampleToTime(void *stream, LONG sample)
{
	return -1;
}

WINFN(HRESULT) avifil_AVIStreamReadFormat(void *stream, LONG position,
					  void *format, LONG *size)
{
	if (size)
		*size = 0;
	return AVIERR_UNSUPPORTED;
}

WINFN(HRESULT) avifil_AVIStreamRead(void *stream, LONG start, LONG samples,
				    void *buffer, LONG buffer_size,
				    LONG *bytes_read, LONG *samples_read)
{
	if (bytes_read)
		*bytes_read = 0;
	if (samples_read)
		*samples_read = 0;
	return AVIERR_UNSUPPORTED;
}

#define E(name) { #name, 0, (void *)avifil_##name }

static const struct builtin_export exports[] = {
	E(AVIFileExit),
	E(AVIFileGetStream),
	E(AVIFileInit),
	E(AVIFileOpenA),
	E(AVIFileOpenW),
	E(AVIFileRelease),
	E(AVIStreamLength),
	E(AVIStreamRead),
	E(AVIStreamReadFormat),
	E(AVIStreamRelease),
	E(AVIStreamSampleToTime),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_avifil32 = { "avifil32.dll", NULL, tables, 0 };
