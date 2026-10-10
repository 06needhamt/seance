/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ICImageCompress and ICImageDecompress: one image in, one packed DIB out
 * (the format, its palette or masks, then the bits) in a GMEM_MOVEABLE
 * block, as the clipboard's CF_DIB holds one. Neither needs the caller to
 * start a sequence first; each runs a Begin/frame/End of its own.
 *
 * ICImageDecompress follows Wine's dlls/msvfw32/msvideo_main.c. Wine only
 * stubs ICImageCompress, so it follows the documented behaviour: the size
 * asked for in *plSize goes to the codec as the frame size to aim for,
 * which it may miss, and *plSize comes back as the size it made.
 */
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/log.h"
#include "vfw/icm.h"
#include "win32/k32.h"

/* The bytes of an uncompressed DIB's pixels. */
static DWORD image_size(const BITMAPINFOHEADER *b)
{
	LONG h = b->biHeight < 0 ? -b->biHeight : b->biHeight;
	DWORD stride = (((DWORD)b->biWidth * b->biBitCount + 31) & ~31u) / 8;

	if (b->biWidth <= 0 || !b->biBitCount)
		return 0;
	return stride * (DWORD)h;
}

/* A malloc()ed copy of a caller's format, with room for a full palette. */
static BITMAPINFOHEADER *copy_format(const BITMAPINFO *bi, size_t *size)
{
	if (bi->bmiHeader.biSize < sizeof(BITMAPINFOHEADER) ||
	    bi->bmiHeader.biSize > 1024)
		return NULL;
	size_t n = vfw_format_size(&bi->bmiHeader);
	BITMAPINFOHEADER *f = calloc(1, n > sizeof(struct vfw_format) ? n :
				     sizeof(struct vfw_format));
	if (f)
		memcpy(f, bi, n);
	*size = n;
	return f;
}

/* A packed DIB: fmt_size bytes of format, then n bytes of bits. */
static HGLOBAL pack(const BITMAPINFOHEADER *fmt, size_t fmt_size,
		    const void *bits, DWORD n)
{
	HGLOBAL h = global_alloc(GMEM_MOVEABLE, fmt_size + n);
	BYTE *p = h ? global_lock(h) : NULL;

	if (!p) {
		global_free(h);
		return NULL;
	}
	memcpy(p, fmt, fmt_size);
	memcpy(p + fmt_size, bits, n);
	((BITMAPINFOHEADER *)p)->biSizeImage = n;
	global_unlock(h);
	return h;
}

HANDLE STUB_WINAPI ic_image_compress(HIC hic, UINT flags, BITMAPINFO *in,
				     void *bits, BITMAPINFO *out_fmt,
				     LONG quality, LONG *size)
{
	BITMAPINFOHEADER *out = NULL;
	BYTE *buf = NULL;
	HGLOBAL h = NULL;
	int opened = 0, begun = 0;
	size_t out_size = 0;
	LRESULT err = ICERR_OK;

	if (flags || !in || !bits) {
		vfw_set_error("ICImageCompress: bad arguments");
		return NULL;
	}
	if (!hic) {
		hic = ic_locate(ICTYPE_VIDEO,
				out_fmt ? out_fmt->bmiHeader.biCompression : 0,
				&in->bmiHeader,
				out_fmt ? &out_fmt->bmiHeader : NULL,
				ICMODE_COMPRESS);
		if (!hic) {
			vfw_set_error("ICImageCompress: no compressor for the "
				      "format");
			return NULL;
		}
		opened = 1;
	}

	if (out_fmt)
		out = copy_format(out_fmt, &out_size);
	else
		out = ic_get_format(hic, ICM_COMPRESS_GET_FORMAT, &in->bmiHeader,
				    &out_size, &err);
	if (!out) {
		vfw_set_error("ICImageCompress: no output format");
		goto done;
	}
	/* A codec refusing its own format here may still compress to it (seq.c). */
	if ((err = ic_send(hic, ICM_COMPRESS_QUERY, (DWORD_PTR)in,
			   (DWORD_PTR)out)) != ICERR_OK && out_fmt) {
		vfw_set_error("ICImageCompress: the codec rejects the formats");
		goto done;
	}

	ic_set_status_proc(hic);
	if ((err = ic_send(hic, ICM_COMPRESS_BEGIN, (DWORD_PTR)in,
			   (DWORD_PTR)out)) != ICERR_OK) {
		vfw_set_error("ICImageCompress: ICM_COMPRESS_BEGIN failed");
		goto done;
	}
	begun = 1;

	/* Asked once begun, as some codecs only know it then (see seq.c). */
	LRESULT max = ic_send(hic, ICM_COMPRESS_GET_SIZE, (DWORD_PTR)in,
			      (DWORD_PTR)out);
	if (max <= 0)
		max = in->bmiHeader.biSizeImage ? in->bmiHeader.biSizeImage :
		      image_size(&in->bmiHeader);
	if (max > 0 && out->biSizeImage > (DWORD)max)
		max = out->biSizeImage;
	if (max <= 0 || !(buf = malloc((size_t)max))) {
		vfw_set_error("ICImageCompress: no room for the output");
		goto done;
	}

	DWORD ckid = 0, out_flags = 0;
	DWORD target = size && *size > 0 ? (DWORD)*size : 0;
	out->biSizeImage = (DWORD)max;
	err = (LRESULT)ic_compress(hic, ICCOMPRESS_KEYFRAME, out, buf,
				   &in->bmiHeader, bits, &ckid, &out_flags, 0,
				   target, (DWORD)quality, NULL, NULL);
	if (err != ICERR_OK || out->biSizeImage > (DWORD)max) {
		vfw_set_error("ICImageCompress: ICCompress failed");
		goto done;
	}
	h = pack(out, out_size, buf, out->biSizeImage);
	if (h && size)
		*size = (LONG)out->biSizeImage;

done:
	if (begun)
		ic_send(hic, ICM_COMPRESS_END, 0, 0);
	if (opened)
		ic_close(hic);
	free(out);
	free(buf);
	return h;
}

HANDLE STUB_WINAPI ic_image_decompress(HIC hic, UINT flags, BITMAPINFO *in,
				       void *bits, BITMAPINFO *out_fmt)
{
	BITMAPINFOHEADER *out = NULL;
	BYTE *pixels = NULL;
	HGLOBAL h = NULL;
	int opened = 0, begun = 0;
	size_t out_size = 0;
	LRESULT err = ICERR_OK;

	if (flags || !in || !bits) {
		vfw_set_error("ICImageDecompress: bad arguments");
		return NULL;
	}
	if (!hic) {
		hic = ic_locate(ICTYPE_VIDEO, 0, &in->bmiHeader,
				out_fmt ? &out_fmt->bmiHeader : NULL,
				ICMODE_DECOMPRESS);
		if (!hic) {
			vfw_set_error("ICImageDecompress: no decompressor for "
				      "the format");
			return NULL;
		}
		opened = 1;
	}

	if (out_fmt) {
		if (out_fmt->bmiHeader.biSize != sizeof(BITMAPINFOHEADER)) {
			vfw_set_error("ICImageDecompress: bad output format");
			goto done;
		}
		out = copy_format(out_fmt, &out_size);
	} else {
		/*
		 * The codec's own choice, with a palette from the codec or, at
		 * the input's depth, the input's own.
		 */
		out = ic_decompress_get_format(hic, &in->bmiHeader, &err);
		if (out && out->biBitCount <= 8) {
			BITMAPINFO *o = (BITMAPINFO *)out;
			if (ic_send(hic, ICM_DECOMPRESS_GET_PALETTE, (DWORD_PTR)in,
				    (DWORD_PTR)out) != ICERR_OK &&
			    in->bmiHeader.biBitCount == out->biBitCount) {
				DWORD n = in->bmiHeader.biClrUsed ?
					  in->bmiHeader.biClrUsed :
					  1u << out->biBitCount;
				memcpy(o->bmiColors, in->bmiColors,
				       sizeof(RGBQUAD) * (n > 256 ? 256 : n));
			}
			if (!out->biClrUsed)
				out->biClrUsed = 1u << out->biBitCount;
		}
		if (out) {
			out->biSize = sizeof(BITMAPINFOHEADER);
			out_size = vfw_format_size(out);
		}
	}
	if (!out) {
		vfw_set_error("ICImageDecompress: no output format");
		goto done;
	}

	DWORD n = out->biSizeImage ? out->biSizeImage : image_size(out);
	if (!n || !(pixels = calloc(1, n))) {
		vfw_set_error("ICImageDecompress: no room for the output");
		goto done;
	}
	if ((err = ic_decompress_begin(hic, &in->bmiHeader, out)) != ICERR_OK) {
		vfw_set_error("ICImageDecompress: ICM_DECOMPRESS_BEGIN failed");
		goto done;
	}
	begun = 1;
	err = (LRESULT)ic_decompress(hic, 0, &in->bmiHeader, bits, out, pixels);
	if (err != ICERR_OK) {
		vfw_set_error("ICImageDecompress: ICDecompress failed");
		goto done;
	}
	h = pack(out, out_size, pixels, n);

done:
	if (begun)
		ic_send(hic, ICM_DECOMPRESS_END, 0, 0);
	if (opened)
		ic_close(hic);
	if (!h) {
		char buf[24];
		TRACE("%s (%s)", vfw_error(), icerr_name(err, buf));
	}
	free(out);
	free(pixels);
	return h;
}
