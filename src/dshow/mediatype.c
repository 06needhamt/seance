/* SPDX-License-Identifier: GPL-2.0-only */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dshow/dshow.h"
#include "win32/ole.h"


static int guid_eq(const GUID *a, const GUID *b)
{
	return !memcmp(a, b, sizeof(*a));
}

static GUID subtype_for(const BITMAPINFOHEADER *b)
{
	if (b->biCompression != BI_RGB && b->biCompression != BI_BITFIELDS)
		return (GUID)FOURCC_SUBTYPE_INIT(b->biCompression);
	switch (b->biBitCount) {
	case 1: return MEDIASUBTYPE_RGB1;
	case 4: return MEDIASUBTYPE_RGB4;
	case 8: return MEDIASUBTYPE_RGB8;
	case 16: return b->biCompression == BI_BITFIELDS ?
		MEDIASUBTYPE_RGB565 : MEDIASUBTYPE_RGB555;
	case 24: return MEDIASUBTYPE_RGB24;
	default: return MEDIASUBTYPE_RGB32;
	}
}

void dshow_free_media_type(AM_MEDIA_TYPE *mt)
{
	if (!mt)
		return;
	if (mt->pUnk)
		mt->pUnk->lpVtbl->Release(mt->pUnk);
	ole_taskmem_free(mt->pbFormat);
	memset(mt, 0, sizeof(*mt));
}

HRESULT dshow_copy_media_type(AM_MEDIA_TYPE *dst, const AM_MEDIA_TYPE *src)
{
	if (!dst || !src)
		return E_POINTER;
	memset(dst, 0, sizeof(*dst));
	*dst = *src;
	dst->pbFormat = NULL;
	if (src->cbFormat) {
		if (!src->pbFormat || !(dst->pbFormat = ole_taskmem_alloc(src->cbFormat)))
			return E_OUTOFMEMORY;
		memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
	}
	if (dst->pUnk)
		dst->pUnk->lpVtbl->AddRef(dst->pUnk);
	return S_OK;
}

HRESULT dshow_bih_to_mt(const BITMAPINFOHEADER *b, size_t size,
			 DWORD rate, DWORD scale, int vi2, AM_MEDIA_TYPE *mt)
{
	if (!b || !mt || size < sizeof(*b) || b->biSize < sizeof(*b) ||
	    b->biSize > size || b->biWidth <= 0 || !b->biHeight ||
	    b->biPlanes != 1)
		return E_INVALIDARG;
	size_t head = vi2 ? offsetof(VIDEOINFOHEADER2, bmiHeader) :
		offsetof(VIDEOINFOHEADER, bmiHeader);
	if (size > UINT32_MAX - head)
		return E_INVALIDARG;
	memset(mt, 0, sizeof(*mt));
	mt->majortype = MEDIATYPE_Video;
	mt->subtype = subtype_for(b);
	mt->bFixedSizeSamples = !b->biCompression || b->biCompression == BI_RGB ||
		b->biCompression == BI_BITFIELDS;
	mt->bTemporalCompression = !mt->bFixedSizeSamples;
	mt->lSampleSize = b->biSizeImage;
	mt->formattype = vi2 ? FORMAT_VideoInfo2 : FORMAT_VideoInfo;
	mt->cbFormat = (ULONG)(head + size);
	if (!(mt->pbFormat = ole_taskmem_alloc(mt->cbFormat)))
		return E_OUTOFMEMORY;
	memset(mt->pbFormat, 0, head);
	memcpy(mt->pbFormat + head, b, size);
	REFERENCE_TIME avg = rate ? (REFERENCE_TIME)10000000 * scale / rate : 0;
	if (vi2) {
		VIDEOINFOHEADER2 *v = (void *)mt->pbFormat;
		v->AvgTimePerFrame = avg;
		v->dwPictAspectRatioX = b->biWidth;
		v->dwPictAspectRatioY = b->biHeight < 0 ? -b->biHeight : b->biHeight;
	} else {
		VIDEOINFOHEADER *v = (void *)mt->pbFormat;
		v->AvgTimePerFrame = avg;
	}
	return S_OK;
}

HRESULT dshow_mt_to_bih(const AM_MEDIA_TYPE *mt, BITMAPINFOHEADER **out,
			 size_t *out_size)
{
	if (!mt || !out)
		return E_POINTER;
	*out = NULL;
	if (!guid_eq(&mt->majortype, &MEDIATYPE_Video))
		return VFW_E_INVALIDMEDIATYPE;
	size_t off;
	if (guid_eq(&mt->formattype, &FORMAT_VideoInfo))
		off = offsetof(VIDEOINFOHEADER, bmiHeader);
	else if (guid_eq(&mt->formattype, &FORMAT_VideoInfo2))
		off = offsetof(VIDEOINFOHEADER2, bmiHeader);
	else if (guid_eq(&mt->formattype, &FORMAT_MPEGVideo))
		off = offsetof(MPEG1VIDEOINFO, hdr.bmiHeader);
	else if (guid_eq(&mt->formattype, &FORMAT_MPEG2Video))
		off = offsetof(MPEG2VIDEOINFO, hdr.bmiHeader);
	else
		return VFW_E_INVALIDMEDIATYPE;
	if (!mt->pbFormat || mt->cbFormat < off + sizeof(BITMAPINFOHEADER))
		return VFW_E_INVALIDMEDIATYPE;
	const BITMAPINFOHEADER *b = (const void *)(mt->pbFormat + off);
	if (b->biSize < sizeof(*b) || b->biSize > mt->cbFormat - off)
		return VFW_E_INVALIDMEDIATYPE;
	size_t n = mt->cbFormat - off;
	BITMAPINFOHEADER *copy = malloc(n);
	if (!copy)
		return E_OUTOFMEMORY;
	memcpy(copy, b, n);
	*out = copy;
	if (out_size)
		*out_size = n;
	return S_OK;
}
