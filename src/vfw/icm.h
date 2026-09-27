/*
 * The msvfw32 ICM API: opening codecs and talking to them. The ic_*
 * functions with Win32 names in comments are what msvfw32.dll exports
 * (see src/win32/msvfw32.c); the rest is a typed C interface for the CLI,
 * mirroring the vfw.h macros (ICDecompressQuery and so on).
 */
#ifndef VFW_VFW_ICM_H
#define VFW_VFW_ICM_H

#include <stddef.h>

#include "runtime/builtin.h"
#include "win32.h"

#define ICTYPE_VIDEO_UPPER mmioFOURCC('V', 'I', 'D', 'C')

/* ICOpen: the codec registered for type.handler; 0 if none opens. */
HIC STUB_WINAPI ic_open(DWORD type, DWORD handler, UINT mode);
/* ICOpenFunction */
HIC STUB_WINAPI ic_open_function(DWORD type, DWORD handler, UINT mode,
				 FARPROC proc);
/* ICClose */
LRESULT STUB_WINAPI ic_close(HIC hic);
/* ICSendMessage: ICERR_BADHANDLE for a bad handle. */
LRESULT STUB_WINAPI ic_send(HIC hic, UINT msg, DWORD_PTR l1, DWORD_PTR l2);
/* ICGetInfo: bytes written, or 0. Fills szDriver if the codec doesn't. */
LRESULT STUB_WINAPI ic_get_info(HIC hic, ICINFO *info, DWORD cb);
/* ICInfo: handler is an index (< 256) or a FOURCC. */
BOOL STUB_WINAPI ic_info(DWORD type, DWORD handler, ICINFO *info);
/* ICLocate */
HIC STUB_WINAPI ic_locate(DWORD type, DWORD handler, BITMAPINFOHEADER *in,
			  BITMAPINFOHEADER *out, WORD flags);
/* ICGetDisplayFormat */
HIC STUB_WINAPI ic_get_display_format(HIC hic, BITMAPINFOHEADER *in,
				      BITMAPINFOHEADER *out, int depth,
				      int dx, int dy);
/* ICInstall / ICRemove (for this process only) */
BOOL STUB_WINAPI ic_install(DWORD type, DWORD handler, LPARAM param,
			    char *desc, UINT flags);
BOOL STUB_WINAPI ic_remove(DWORD type, DWORD handler, UINT flags);
/* ICCompress (cdecl) */
DWORD STUB_CDECL ic_compress(HIC hic, DWORD flags, BITMAPINFOHEADER *out_fmt,
			     void *out, BITMAPINFOHEADER *in_fmt, void *in,
			     DWORD *ckid, DWORD *out_flags, LONG frame,
			     DWORD frame_size, DWORD quality,
			     BITMAPINFOHEADER *prev_fmt, void *prev);
/* ICDecompress (cdecl) */
DWORD STUB_CDECL ic_decompress(HIC hic, DWORD flags,
			       BITMAPINFOHEADER *in_fmt, void *in,
			       BITMAPINFOHEADER *out_fmt, void *out);

/*
 * Opens the codec in a DLL directly (not through the registry), sending
 * type.handler in its ICOPEN. 0 with vfw_error() if it doesn't open.
 */
HIC ic_open_dll(const char *path, DWORD type, DWORD handler, UINT mode);

/* The DLL path or name behind a HIC ("" for a function). */
const char *ic_driver_name(HIC hic);

/* Whether hic is an open codec. */
int ic_valid(HIC hic);

/* A name for an ICERR_* value ("ICERR_BADFORMAT"). */
const char *icerr_name(LRESULT err, char buf[24]);

/* The size of a format: header plus palette or masks. */
size_t vfw_format_size(const BITMAPINFOHEADER *bih);

/* A BITMAPINFOHEADER with room for a palette or masks. */
struct vfw_format {
	BITMAPINFOHEADER bih;
	RGBQUAD colors[256];
};

/*
 * The i-th of the standard uncompressed formats tried when negotiating
 * (I420, YV12, YUY2, UYVY, BGR32, BGR24, RGB565, RGB555, PAL8), at w x h.
 * Returns its name, or NULL past the end.
 */
const char *vfw_std_format(unsigned i, LONG w, LONG h, struct vfw_format *f);

/*
 * Picks a decompressed format for input `in`: the codec's own suggestion
 * if it accepts it, else the first standard format it accepts. Returns a
 * malloc()ed format, or NULL with *err set to an ICERR_* value.
 */
BITMAPINFOHEADER *ic_negotiate_output(HIC hic, const BITMAPINFOHEADER *in,
				      LRESULT *err);

/*
 * ICM_DECOMPRESS_GET_FORMAT and ICM_COMPRESS_GET_FORMAT: the size query,
 * then the format, malloc()ed; NULL with *err set on failure.
 */
BITMAPINFOHEADER *ic_decompress_get_format(HIC hic, const BITMAPINFOHEADER *in,
					   LRESULT *err);
BITMAPINFOHEADER *ic_compress_get_format(HIC hic, const BITMAPINFOHEADER *in,
					 LRESULT *err);

#endif /* VFW_VFW_ICM_H */
