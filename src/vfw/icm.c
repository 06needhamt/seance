/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The msvfw32 ICM API, after Wine's dlls/msvfw32/msvideo_main.c.
 */
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/fourcc.h"
#include "common/log.h"
#include "out/image.h"
#include "runtime/runtime.h"
#include "vfw/driver.h"
#include "vfw/icm.h"
#include "vfw/registry.h"
#include "win32/path.h"

#define MAX_HICS   256
#define HIC_TAG    0x5c000000u
#define MAX_FORMAT (1u << 20)	/* the largest format a codec may return */

struct ic {
	HDRVR drv;		/* NULL when free */
	FOURCC type, handler;
	UINT mode;
	char driver[256];	/* DLL path, or "" for a function */
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct ic ics[MAX_HICS];

static int slot_of(HIC hic)
{
	UINT_PTR v = (UINT_PTR)hic;
	unsigned i = (unsigned)(v & 0xffff) - 1;

	if ((v & 0xffff0000u) != HIC_TAG || i >= MAX_HICS || !ics[i].drv)
		return -1;
	return (int)i;
}

int ic_valid(HIC hic)
{
	pthread_mutex_lock(&lock);
	int r = slot_of(hic) >= 0;
	pthread_mutex_unlock(&lock);
	return r;
}

static HDRVR drv_of(HIC hic)
{
	pthread_mutex_lock(&lock);
	int s = slot_of(hic);
	HDRVR d = s >= 0 ? ics[s].drv : NULL;
	pthread_mutex_unlock(&lock);
	return d;
}

const char *ic_driver_name(HIC hic)
{
	pthread_mutex_lock(&lock);
	int s = slot_of(hic);
	const char *n = s >= 0 ? ics[s].driver : "";
	pthread_mutex_unlock(&lock);
	return n;
}

/* Records an open driver as a HIC; closes it if the table is full. */
static HIC add_ic(HDRVR drv, FOURCC type, FOURCC handler, UINT mode,
		  const char *driver)
{
	int slot = -1;

	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAX_HICS && slot < 0; i++)
		if (!ics[i].drv) {
			slot = i;
			ics[i].drv = drv;
			ics[i].type = type;
			ics[i].handler = handler;
			ics[i].mode = mode;
			snprintf(ics[i].driver, sizeof(ics[i].driver), "%s",
				 driver ? driver : "");
		}
	pthread_mutex_unlock(&lock);
	if (slot < 0) {
		drv_close(drv, 0, 0);
		vfw_set_error("too many open codecs");
		return NULL;
	}
	return (HIC)(UINT_PTR)(HIC_TAG | (unsigned)(slot + 1));
}

/* "vidc.XXXX" as UTF-16, the name a codec gets in DRV_OPEN. */
static void alias_of(FOURCC type, FOURCC handler, WCHAR out[10])
{
	for (int i = 0; i < 4; i++) {
		out[i] = (type >> (8 * i)) & 0xff;
		out[5 + i] = (handler >> (8 * i)) & 0xff;
	}
	out[4] = '.';
	out[9] = 0;
}

static ICOPEN make_icopen(FOURCC type, FOURCC handler, UINT mode)
{
	ICOPEN op;

	memset(&op, 0, sizeof(op));
	op.dwSize = sizeof(op);
	op.fccType = type;
	op.fccHandler = handler;
	op.dwVersion = ICVERSION;
	op.dwFlags = mode;
	return op;
}

HIC STUB_WINAPI ic_open(DWORD type, DWORD handler, UINT mode)
{
	char fcc[FOURCC_STR_SIZE], path[PATH_MAX];
	WCHAR alias[10];

	if (runtime_enter())
		return NULL;
	const struct vfw_codec *c = vfw_find(type, handler);
	if (!c) {
		vfw_set_error("no codec is registered for '%s'",
			      fourcc_str(handler, fcc));
		TRACE("ICOpen: %s", vfw_error());
		return NULL;
	}
	/* Copy what's needed: the codec may change the registry. */
	FOURCC rtype = c->type, rhandler = c->handler;
	DRIVERPROC proc = c->proc;
	snprintf(path, sizeof(path), "%s", c->path ? c->path : "");

	ICOPEN op = make_icopen(type, handler, mode);
	alias_of(rtype, rhandler, alias);
	HDRVR drv = proc ? drv_open_proc(proc, alias, (LPARAM)&op) :
			   drv_open(path, alias, (LPARAM)&op);
	if (!drv) {
		TRACE("ICOpen '%s': %s", fourcc_str(handler, fcc), vfw_error());
		return NULL;
	}
	return add_ic(drv, type, handler, mode, path);
}

HIC ic_open_dll(const char *path, DWORD type, DWORD handler, UINT mode)
{
	WCHAR alias[10];

	ICOPEN op = make_icopen(type, handler, mode);
	alias_of(type, handler, alias);
	HDRVR drv = drv_open(path, handler ? alias : NULL, (LPARAM)&op);
	if (!drv)
		return NULL;
	return add_ic(drv, type, handler, mode, path);
}

HIC STUB_WINAPI ic_open_function(DWORD type, DWORD handler, UINT mode,
				 FARPROC proc)
{
	WCHAR alias[10];

	ICOPEN op = make_icopen(type, handler, mode);
	alias_of(type, handler, alias);
	HDRVR drv = drv_open_proc((DRIVERPROC)(void *)proc, alias, (LPARAM)&op);
	if (!drv)
		return NULL;
	return add_ic(drv, type, handler, mode, NULL);
}

LRESULT STUB_WINAPI ic_close(HIC hic)
{
	pthread_mutex_lock(&lock);
	int s = slot_of(hic);
	HDRVR drv = s >= 0 ? ics[s].drv : NULL;
	if (s >= 0)
		ics[s].drv = NULL;
	pthread_mutex_unlock(&lock);
	if (!drv)
		return ICERR_BADHANDLE;
	drv_close(drv, 0, 0);
	return ICERR_OK;
}

LRESULT STUB_WINAPI ic_send(HIC hic, UINT msg, DWORD_PTR l1, DWORD_PTR l2)
{
	HDRVR drv = drv_of(hic);

	if (!drv)
		return ICERR_BADHANDLE;
	return drv_send(drv, msg, (LPARAM)l1, (LPARAM)l2);
}

static LONG CALLBACK status_proc(LPARAM l, UINT message, LONG value)
{
	return 0;
}

void ic_set_status_proc(HIC hic)
{
	ICSETSTATUSPROC status = { .Status = status_proc };

	ic_send(hic, ICM_SET_STATUS_PROC, (DWORD_PTR)&status, sizeof(status));
}

/* szDriver: the DLL's file name, as Windows fills it in. */
static void set_driver_name(ICINFO *info, const char *path)
{
	const char *base = strrchr(path, '/');
	size_t i = 0;

	base = base ? base + 1 : path;
	for (; base[i] && i < 127; i++)
		info->szDriver[i] = (BYTE)base[i];
	info->szDriver[i] = 0;
}

LRESULT STUB_WINAPI ic_get_info(HIC hic, ICINFO *info, DWORD cb)
{
	if (!info || !ic_valid(hic))
		return 0;
	if (cb >= sizeof(*info))
		info->szDriver[0] = 0;
	LRESULT r = ic_send(hic, ICM_GETINFO, (DWORD_PTR)info, cb);
	if (cb >= sizeof(*info) && !info->szDriver[0])
		set_driver_name(info, ic_driver_name(hic));
	return r;
}

BOOL STUB_WINAPI ic_info(DWORD type, DWORD handler, ICINFO *info)
{
	if (!info)
		return FALSE;
	/* A FOURCC is at least four printable bytes; below that, an index. */
	const struct vfw_codec *c = handler < 256 ?
		vfw_enum(type == ICTYPE_VIDEO_UPPER ? ICTYPE_VIDEO : type, handler) :
		vfw_find(type, handler);
	if (!c)
		return FALSE;
	memset(info, 0, sizeof(*info));
	info->dwSize = sizeof(*info);
	info->fccType = c->type;
	info->fccHandler = c->handler;
	info->dwVersionICM = ICVERSION;
	set_driver_name(info, c->path ? c->path : "");
	return TRUE;
}

static UINT query_msg(WORD flags)
{
	switch (flags) {
	case ICMODE_COMPRESS:
	case ICMODE_FASTCOMPRESS:
		return ICM_COMPRESS_QUERY;
	case ICMODE_DECOMPRESS:
	case ICMODE_FASTDECOMPRESS:
		return ICM_DECOMPRESS_QUERY;
	case ICMODE_DRAW:
		return ICM_DRAW_QUERY;
	}
	return 0;
}

static HIC try_locate(DWORD type, DWORD handler, UINT msg, WORD flags,
		      BITMAPINFOHEADER *in, BITMAPINFOHEADER *out)
{
	HIC hic = ic_open(type, handler, flags);

	if (hic && ic_send(hic, msg, (DWORD_PTR)in, (DWORD_PTR)out) == ICERR_OK)
		return hic;
	if (hic)
		ic_close(hic);
	return NULL;
}

HIC STUB_WINAPI ic_locate(DWORD type, DWORD handler, BITMAPINFOHEADER *in,
			  BITMAPINFOHEADER *out, WORD flags)
{
	UINT msg = query_msg(flags);
	HIC hic = NULL;

	if (!msg || !in)
		return NULL;
	if (handler)
		hic = try_locate(type, handler, msg, flags, in, out);
	if (!hic && msg != ICM_COMPRESS_QUERY && in->biCompression &&
	    in->biCompression != handler)
		hic = try_locate(type, in->biCompression, msg, flags, in, out);
	for (unsigned i = 0; !hic; i++) {
		const struct vfw_codec *c = vfw_enum(type, i);
		if (!c)
			break;
		FOURCC h = c->handler;
		if (h != handler)
			hic = try_locate(type, h, msg, flags, in, out);
	}
	TRACE("ICLocate -> %p", (void *)hic);
	return hic;
}

HIC STUB_WINAPI ic_get_display_format(HIC hic, BITMAPINFOHEADER *in,
				      BITMAPINFOHEADER *out, int depth,
				      int dx, int dy)
{
	static const int depths[] = { 32, 24, 16 };
	HIC own = NULL;

	if (!in || !out)
		return NULL;
	if (!hic && !(hic = own = ic_locate(ICTYPE_VIDEO, 0, in, NULL,
					    ICMODE_DECOMPRESS)))
		return NULL;
	if (ic_send(hic, ICM_DECOMPRESS_QUERY, (DWORD_PTR)in, 0) != ICERR_OK)
		goto fail;
	if (!dx)
		dx = in->biWidth;
	if (!dy)
		dy = in->biHeight < 0 ? -in->biHeight : in->biHeight;
	for (int i = -1; i < 3; i++) {
		int d = i < 0 ? depth : depths[i];
		if (d != 16 && d != 24 && d != 32)
			continue;
		memset(out, 0, sizeof(*out));
		out->biSize = sizeof(*out);
		out->biWidth = dx;
		out->biHeight = dy;
		out->biPlanes = 1;
		out->biBitCount = d;
		out->biCompression = BI_RGB;
		out->biSizeImage = ((dx * d + 31) / 32 * 4) * dy;
		if (ic_send(hic, ICM_DECOMPRESS_QUERY, (DWORD_PTR)in,
			    (DWORD_PTR)out) == ICERR_OK)
			return hic;
	}
fail:
	if (own)
		ic_close(own);
	return NULL;
}

BOOL STUB_WINAPI ic_install(DWORD type, DWORD handler, LPARAM param,
			    char *desc, UINT flags)
{
	if (flags & ICINSTALL_FUNCTION)
		return !vfw_register_proc(type, handler, (DRIVERPROC)param);
	if (!(flags & ICINSTALL_DRIVER) || !param)
		return FALSE;

	char name[PATH_MAX];
	if (flags & ICINSTALL_UNICODE) {
		const WCHAR *w = (const WCHAR *)param;
		size_t i = 0;
		for (; w[i] && i < sizeof(name) - 1; i++)
			name[i] = w[i] < 0x80 ? (char)w[i] : '?';
		name[i] = '\0';
	} else {
		snprintf(name, sizeof(name), "%s", (const char *)param);
	}
	char *unix_path = (strchr(name, ':') || strchr(name, '\\')) ?
			  path_to_unix(name) : NULL;
	int r = vfw_register(type, handler, unix_path ? unix_path : name);
	free(unix_path);
	return !r;
}

BOOL STUB_WINAPI ic_remove(DWORD type, DWORD handler, UINT flags)
{
	return !vfw_unregister(type, handler);
}

DWORD STUB_CDECL ic_compress(HIC hic, DWORD flags, BITMAPINFOHEADER *out_fmt,
			     void *out, BITMAPINFOHEADER *in_fmt, void *in,
			     DWORD *ckid, DWORD *out_flags, LONG frame,
			     DWORD frame_size, DWORD quality,
			     BITMAPINFOHEADER *prev_fmt, void *prev)
{
	ICCOMPRESS c = {
		.dwFlags = flags, .lpbiOutput = out_fmt, .lpOutput = out,
		.lpbiInput = in_fmt, .lpInput = in, .lpckid = ckid,
		.lpdwFlags = out_flags, .lFrameNum = frame,
		.dwFrameSize = frame_size, .dwQuality = quality,
		.lpbiPrev = prev_fmt, .lpPrev = prev,
	};
	return ic_send(hic, ICM_COMPRESS, (DWORD_PTR)&c, sizeof(c));
}

DWORD STUB_CDECL ic_decompress(HIC hic, DWORD flags,
			       BITMAPINFOHEADER *in_fmt, void *in,
			       BITMAPINFOHEADER *out_fmt, void *out)
{
	ICDECOMPRESS d = {
		.dwFlags = flags, .lpbiInput = in_fmt, .lpInput = in,
		.lpbiOutput = out_fmt, .lpOutput = out, .ckid = 0,
	};
	return ic_send(hic, ICM_DECOMPRESS, (DWORD_PTR)&d, sizeof(d));
}

const char *icerr_name(LRESULT err, char buf[24])
{
	switch (err) {
	case ICERR_OK:           return "ICERR_OK";
	case ICERR_DONTDRAW:     return "ICERR_DONTDRAW";
	case ICERR_NEWPALETTE:   return "ICERR_NEWPALETTE";
	case ICERR_GOTOKEYFRAME: return "ICERR_GOTOKEYFRAME";
	case ICERR_STOPDRAWING:  return "ICERR_STOPDRAWING";
	case ICERR_UNSUPPORTED:  return "ICERR_UNSUPPORTED";
	case ICERR_BADFORMAT:    return "ICERR_BADFORMAT";
	case ICERR_MEMORY:       return "ICERR_MEMORY";
	case ICERR_INTERNAL:     return "ICERR_INTERNAL";
	case ICERR_BADFLAGS:     return "ICERR_BADFLAGS";
	case ICERR_BADPARAM:     return "ICERR_BADPARAM";
	case ICERR_BADSIZE:      return "ICERR_BADSIZE";
	case ICERR_BADHANDLE:    return "ICERR_BADHANDLE";
	case ICERR_CANTUPDATE:   return "ICERR_CANTUPDATE";
	case ICERR_ABORT:        return "ICERR_ABORT";
	case ICERR_ERROR:        return "ICERR_ERROR";
	case ICERR_BADBITDEPTH:  return "ICERR_BADBITDEPTH";
	case ICERR_BADIMAGESIZE: return "ICERR_BADIMAGESIZE";
	}
	if (err <= ICERR_CUSTOM)
		snprintf(buf, 24, "ICERR_CUSTOM%+ld", (long)(err - ICERR_CUSTOM));
	else
		snprintf(buf, 24, "%ld", (long)err);
	return buf;
}

size_t vfw_format_size(const BITMAPINFOHEADER *bih)
{
	size_t n = bih->biSize;

	if (bih->biCompression == BI_BITFIELDS && bih->biSize < 52)
		n += 12;
	else if (bih->biBitCount <= 8 &&
		 (bih->biCompression == BI_RGB || bih->biCompression == BI_RLE8 ||
		  bih->biCompression == BI_RLE4)) {
		size_t colors = bih->biClrUsed ? bih->biClrUsed : 1u << bih->biBitCount;
		n += 4 * (colors > 256 ? 256 : colors);
	}
	return n;
}

const char *vfw_std_format(unsigned i, LONG w, LONG h, struct vfw_format *f)
{
	static const struct {
		const char *name;
		DWORD comp;
		WORD bits;
	} fmts[] = {
		{ "I420", mmioFOURCC('I', '4', '2', '0'), 12 },
		{ "YV12", mmioFOURCC('Y', 'V', '1', '2'), 12 },
		{ "YUY2", mmioFOURCC('Y', 'U', 'Y', '2'), 16 },
		{ "UYVY", mmioFOURCC('U', 'Y', 'V', 'Y'), 16 },
		{ "BGR32", BI_RGB, 32 },
		{ "BGR24", BI_RGB, 24 },
		{ "RGB565", BI_BITFIELDS, 16 },
		{ "RGB555", BI_RGB, 16 },
		{ "PAL8", BI_RGB, 8 },
	};

	if (i >= sizeof(fmts) / sizeof(fmts[0]))
		return NULL;
	memset(f, 0, sizeof(*f));
	BITMAPINFOHEADER *b = &f->bih;
	b->biSize = sizeof(*b);
	b->biWidth = w;
	b->biHeight = h;
	b->biPlanes = 1;
	b->biBitCount = fmts[i].bits;
	b->biCompression = fmts[i].comp;
	if (fmts[i].bits == 12)
		b->biSizeImage = (DWORD)image_i420_size(w, h);
	else if (fmts[i].comp > BI_BITFIELDS)
		b->biSizeImage = (DWORD)((w + 1) / 2 * 4 * h);
	else
		b->biSizeImage = (DWORD)((w * fmts[i].bits + 31) / 32 * 4 * h);
	if (fmts[i].comp == BI_BITFIELDS) {
		DWORD masks[3] = { 0xf800, 0x07e0, 0x001f };
		memcpy(f->colors, masks, sizeof(masks));
	} else if (fmts[i].bits == 8) {
		b->biClrUsed = 256;
		for (int c = 0; c < 256; c++)
			f->colors[c] = (RGBQUAD){ c, c, c, 0 };
	}
	return fmts[i].name;
}

BITMAPINFOHEADER *ic_get_format(HIC hic, UINT msg, const BITMAPINFOHEADER *in,
				 size_t *fmt_size, LRESULT *err)
{
	LRESULT asked = ic_send(hic, msg, (DWORD_PTR)in, 0), size = asked;

	/* Some codecs answer the size query with 0: room for any plain format. */
	if (size == 0)
		size = sizeof(struct vfw_format);
	if (size < (LRESULT)sizeof(BITMAPINFOHEADER) || size > (LRESULT)MAX_FORMAT) {
		*err = size < 0 ? size : ICERR_BADSIZE;
		return NULL;
	}
	size_t cap = (size_t)size > sizeof(struct vfw_format) ?
		     (size_t)size : sizeof(struct vfw_format);
	BITMAPINFOHEADER *out = calloc(1, cap);
	if (!out) {
		*err = ICERR_MEMORY;
		return NULL;
	}
	LRESULT r = ic_send(hic, msg, (DWORD_PTR)in, (DWORD_PTR)out);
	if (r < 0) {
		*err = r;
		free(out);
		return NULL;
	}
	if (out->biSize < sizeof(*out) || out->biSize > cap)
		out->biSize = sizeof(*out);
	if (fmt_size) {
		/*
		 * What the codec said, or what the header implies if more; with
		 * no answer, only the header's size (not the room given).
		 */
		size_t n = vfw_format_size(out);
		if (n > cap)
			n = cap;
		*fmt_size = asked > 0 && (size_t)asked > n ? (size_t)asked : n;
	}
	*err = ICERR_OK;
	return out;
}

BITMAPINFOHEADER *ic_decompress_get_format(HIC hic, const BITMAPINFOHEADER *in,
					   LRESULT *err)
{
	return ic_get_format(hic, ICM_DECOMPRESS_GET_FORMAT, in, NULL, err);
}

LRESULT ic_decompress_begin(HIC hic, const BITMAPINFOHEADER *in,
			    const BITMAPINFOHEADER *out)
{
	LRESULT r;

	free(ic_decompress_get_format(hic, in, &r));
	return ic_send(hic, ICM_DECOMPRESS_BEGIN, (DWORD_PTR)in, (DWORD_PTR)out);
}

BITMAPINFOHEADER *ic_compress_get_format(HIC hic, const BITMAPINFOHEADER *in,
					 LRESULT *err)
{
	return ic_get_format(hic, ICM_COMPRESS_GET_FORMAT, in, NULL, err);
}

BITMAPINFOHEADER *ic_negotiate_output(HIC hic, const BITMAPINFOHEADER *in,
				      LRESULT *err)
{
	BITMAPINFOHEADER *out = ic_decompress_get_format(hic, in, err);
	struct vfw_format f;

	if (out && ic_send(hic, ICM_DECOMPRESS_QUERY, (DWORD_PTR)in,
			   (DWORD_PTR)out) == ICERR_OK)
		return out;
	free(out);

	LONG h = in->biHeight < 0 ? -in->biHeight : in->biHeight;
	for (unsigned i = 0; vfw_std_format(i, in->biWidth, h, &f); i++)
		if (ic_send(hic, ICM_DECOMPRESS_QUERY, (DWORD_PTR)in,
			    (DWORD_PTR)&f.bih) == ICERR_OK) {
			if (!(out = malloc(sizeof(f)))) {
				*err = ICERR_MEMORY;
				return NULL;
			}
			memcpy(out, &f, sizeof(f));
			*err = ICERR_OK;
			return out;
		}
	*err = ICERR_BADFORMAT;
	return NULL;
}
