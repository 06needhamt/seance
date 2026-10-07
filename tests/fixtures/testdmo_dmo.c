/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * testdmo.dll's DMOs: a TST0 decoder and encoder, CRT-less, with the same
 * lossless bitstream as testcodec.dll and testfilter.ax, so all three can
 * be cross-checked byte for byte. A TST0 frame is 'K' (a key frame) or 'D'
 * (the XOR difference from the previous picture), the raw format's bit
 * depth and two zero bytes, then the picture run-length coded.
 *
 * Like most DMOs, ProcessInput keeps the buffer it is given and
 * ProcessOutput turns it into output. Modes, from environment variables
 * set to 1, cover the ways DMOs differ:
 *   TESTDMO_DELAY        the decoder gives each picture one input late,
 *                        unstamped, and the last one on Discontinuity
 *   TESTDMO_BURST        the decoder gives pictures in pairs, the first
 *                        flagged DMO_OUTPUT_DATA_BUFFERF_INCOMPLETE
 *   TESTDMO_NOTACCEPTING every other ProcessInput is refused with
 *                        DMO_E_NOTACCEPTING, as by a busy DMO
 *   TESTDMO_SWITCH       from the second picture on, the decoder's I420
 *                        output becomes YV12, as GetOutputCurrentType says
 *   TESTDMO_PRIVATE      the encoder has IWMCodecPrivateData, giving four
 *                        bytes of codec data for the stream header
 *   TESTDMO_STRICT       every pointer handed in must be below 2 GB
 */
#include "fixture.h"
#include <dmoreg.h>
#include <mediaerr.h>
#include <strmif.h>
#include <amvideo.h>

/* IWMCodecPrivateData, which mingw-w64 doesn't declare. */
typedef struct IWMCodecPrivateData IWMCodecPrivateData;
typedef struct {
	HRESULT (WINAPI *QueryInterface)(IWMCodecPrivateData *, REFIID, void **);
	ULONG (WINAPI *AddRef)(IWMCodecPrivateData *);
	ULONG (WINAPI *Release)(IWMCodecPrivateData *);
	HRESULT (WINAPI *SetPartialOutputType)(IWMCodecPrivateData *,
					       DMO_MEDIA_TYPE *);
	HRESULT (WINAPI *GetPrivateData)(IWMCodecPrivateData *, BYTE *,
					 DWORD *);
} IWMCodecPrivateDataVtbl;
struct IWMCodecPrivateData { const IWMCodecPrivateDataVtbl *lpVtbl; };

#define FOURCC(a, b, c, d) \
	((DWORD)(a) | (DWORD)(b) << 8 | (DWORD)(c) << 16 | (DWORD)(d) << 24)
#define FOURCC_GUID(f) { (f), 0x0000, 0x0010, \
	{ 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } }

static const GUID iid_unknown = { 0, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID iid_media_object = { 0xd8ad0f58, 0x5494, 0x4102,
	{ 0x97, 0xc5, 0xec, 0x79, 0x8e, 0x59, 0xbc, 0xf4 } };
static const GUID iid_persist_stream = { 0x00000109, 0, 0,
	{ 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID iid_codec_api = { 0x901db4c7, 0x31ce, 0x41a2,
	{ 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda } };
static const GUID iid_private_data = { 0x73f0be8e, 0x57f7, 0x4f01,
	{ 0xaa, 0x66, 0x9f, 0x57, 0x34, 0x0c, 0xfe, 0x0e } };
static const GUID api_gop_size = { 0x95f31b26, 0x95a4, 0x41aa,
	{ 0x93, 0x03, 0x24, 0x6a, 0x7f, 0xc6, 0xee, 0xf1 } };
static const GUID format_video_info = { 0x05589f80, 0xc356, 0x11ce,
	{ 0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a } };
static const GUID mt_video = FOURCC_GUID(FOURCC('v', 'i', 'd', 's'));
static const GUID st_tst0 = FOURCC_GUID(FOURCC('T', 'S', 'T', '0'));
static const GUID st_rgb24 = { 0xe436eb7d, 0x524f, 0x11ce,
	{ 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
static const GUID st_rgb32 = { 0xe436eb7e, 0x524f, 0x11ce,
	{ 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };

#define TST0 FOURCC('T', 'S', 'T', '0')
#define I420 FOURCC('I', '4', '2', '0')
#define YV12 FOURCC('Y', 'V', '1', '2')
#define YUY2 FOURCC('Y', 'U', 'Y', '2')
#define UYVY FOURCC('U', 'Y', 'V', 'Y')

/* The raw formats: name (BI_RGB for RGB), depth. */
static const struct { DWORD compression; WORD bits; } raw_formats[] = {
	{ I420, 12 }, { YV12, 12 }, { YUY2, 16 }, { UYVY, 16 },
	{ BI_RGB, 24 }, { BI_RGB, 32 },
};
#define NRAW (sizeof(raw_formats) / sizeof(*raw_formats))

struct dmo {
	IMediaObject iface;
	IPersistStream persist;
	ICodecAPI api;
	IWMCodecPrivateData pd;
	LONG refs;
	BOOL encoder;
	BOOL delay, burst, refuse, switching, private_data, strict;
	DMO_MEDIA_TYPE in, out;
	BOOL in_set, out_set;
	BYTE *picture;			/* the previous picture */
	DWORD picture_size, frame, inputs;
	BOOL valid;
	LONG key_interval;
	/* the input held, and its flags and times */
	IMediaBuffer *pending;
	DWORD pending_flags;
	REFERENCE_TIME pending_time, pending_length;
	/* decoded pictures not yet given out (DELAY, BURST) */
	BYTE *held[2];
	DWORD nheld;
	BOOL draining, bursting;
};

static LONG objects;

static int eq(const GUID *a, const GUID *b)
{
	const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;
	for (int i = 0; i < 16; i++)
		if (x[i] != y[i])
			return 0;
	return 1;
}

static void *alloc(DWORD n)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n);
}

static void release(void *p)
{
	if (p)
		HeapFree(GetProcessHeap(), 0, p);
}

static void copy(void *d, const void *s, DWORD n)
{
	for (DWORD i = 0; i < n; i++)
		((BYTE *)d)[i] = ((const BYTE *)s)[i];
}

static BOOL high(const struct dmo *d, const void *p)
{
	return d->strict && (ULONG_PTR)p >= 0x80000000UL;
}
#define LOWPTR(d, p) do { if (high((d), (p))) return E_POINTER; } while (0)

static BOOL env_on(const char *name)
{
	char value[2];
	return GetEnvironmentVariableA(name, value, sizeof(value)) == 1 &&
	       value[0] == '1';
}

/* Media types. */

static void free_type(DMO_MEDIA_TYPE *m)
{
	if (m->pUnk)
		m->pUnk->lpVtbl->Release(m->pUnk);
	CoTaskMemFree(m->pbFormat);
	for (DWORD i = 0; i < sizeof(*m); i++)
		((BYTE *)m)[i] = 0;
}

static HRESULT copy_type(DMO_MEDIA_TYPE *d, const DMO_MEDIA_TYPE *s)
{
	*d = *s;
	d->pUnk = NULL;
	d->pbFormat = NULL;
	if (s->cbFormat) {
		if (!(d->pbFormat = CoTaskMemAlloc(s->cbFormat)))
			return E_OUTOFMEMORY;
		copy(d->pbFormat, s->pbFormat, s->cbFormat);
	}
	return S_OK;
}

static BITMAPINFOHEADER *type_bih(const DMO_MEDIA_TYPE *mt)
{
	if (!mt->pbFormat || mt->cbFormat < sizeof(VIDEOINFOHEADER) ||
	    !eq(&mt->formattype, &format_video_info))
		return NULL;
	return &((VIDEOINFOHEADER *)mt->pbFormat)->bmiHeader;
}

static LONG abs_long(LONG n)
{
	return n < 0 ? -n : n;
}

static DWORD frame_bytes(const BITMAPINFOHEADER *b)
{
	LONG h = abs_long(b->biHeight);
	if (b->biBitCount == 12)
		return (DWORD)(b->biWidth * h +
			       2 * ((b->biWidth + 1) / 2) * ((h + 1) / 2));
	return ((DWORD)b->biWidth * b->biBitCount + 31) / 32 * 4 * h;
}

static int raw_index(const BITMAPINFOHEADER *b)
{
	for (DWORD i = 0; i < NRAW; i++)
		if (b->biCompression == raw_formats[i].compression &&
		    b->biBitCount == raw_formats[i].bits)
			return (int)i;
	return -1;
}

/* A whole VideoInfo type: the given picture, as a raw format or TST0. */
static HRESULT make_type(DMO_MEDIA_TYPE *mt, const BITMAPINFOHEADER *like,
			 DWORD compression, WORD bits)
{
	VIDEOINFOHEADER *vih = CoTaskMemAlloc(sizeof(*vih));

	for (DWORD i = 0; i < sizeof(*mt); i++)
		((BYTE *)mt)[i] = 0;
	if (!vih)
		return E_OUTOFMEMORY;
	for (DWORD i = 0; i < sizeof(*vih); i++)
		((BYTE *)vih)[i] = 0;
	vih->bmiHeader = *like;
	vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	vih->bmiHeader.biCompression = compression;
	vih->bmiHeader.biBitCount = bits;
	vih->bmiHeader.biPlanes = 1;
	DWORD raw = frame_bytes(&vih->bmiHeader);
	vih->bmiHeader.biSizeImage = compression == TST0 ? raw * 2 + 4096 : raw;
	mt->majortype = mt_video;
	if (compression == TST0)
		mt->subtype = st_tst0;
	else if (compression == BI_RGB)
		mt->subtype = bits == 24 ? st_rgb24 : st_rgb32;
	else
		mt->subtype = (GUID)FOURCC_GUID(compression);
	mt->bFixedSizeSamples = compression != TST0;
	mt->bTemporalCompression = compression == TST0;
	mt->lSampleSize = compression == TST0 ? 0 : raw;
	mt->formattype = format_video_info;
	mt->cbFormat = sizeof(*vih);
	mt->pbFormat = (BYTE *)vih;
	return S_OK;
}

/* The run-length coding of testfilter_filter.c. */
static int run_at(const BYTE *s, DWORD i, DWORD n)
{
	return i + 2 < n && s[i] == s[i + 1] && s[i] == s[i + 2];
}

static DWORD rle_encode(const BYTE *s, DWORD n, BYTE *d)
{
	DWORD i = 0, o = 0;
	while (i < n) {
		if (run_at(s, i, n)) {
			DWORD run = 3;
			while (i + run < n && s[i + run] == s[i] && run < 129)
				run++;
			d[o++] = (BYTE)(run + 126);
			d[o++] = s[i];
			i += run;
		} else {
			DWORD begin = i, lit = 0;
			while (i < n && lit < 128 && (lit == 0 || !run_at(s, i, n))) {
				i++;
				lit++;
			}
			d[o++] = (BYTE)(lit - 1);
			for (DWORD k = 0; k < lit; k++)
				d[o++] = s[begin + k];
		}
	}
	return o;
}

static int rle_decode(const BYTE *s, DWORD len, BYTE *d, DWORD n, BOOL delta)
{
	DWORD i = 0, o = 0;
	while (o < n) {
		if (i >= len)
			return -1;
		BYTE c = s[i++];
		if (c < 128) {
			DWORD lit = c + 1;
			if (i + lit > len || o + lit > n)
				return -1;
			for (DWORD k = 0; k < lit; k++, o++)
				d[o] = delta ? (BYTE)(d[o] ^ s[i + k]) : s[i + k];
			i += lit;
		} else {
			DWORD run = c - 126;
			if (i >= len || o + run > n)
				return -1;
			for (DWORD k = 0; k < run; k++, o++)
				d[o] = delta ? (BYTE)(d[o] ^ s[i]) : s[i];
			i++;
		}
	}
	return 0;
}

/* IMediaObject. */

static struct dmo *from(IMediaObject *p)
{
	return (struct dmo *)p;
}

static HRESULT WINAPI dmo_qi(IMediaObject *p, REFIID iid, void **out)
{
	struct dmo *d = from(p);

	if (!out)
		return E_POINTER;
	if (eq(iid, &iid_unknown) || eq(iid, &iid_media_object))
		*out = p;
	else if (d->encoder && eq(iid, &iid_persist_stream))
		*out = &d->persist;
	else if (d->encoder && eq(iid, &iid_codec_api))
		*out = &d->api;
	else if (d->encoder && d->private_data && eq(iid, &iid_private_data))
		*out = &d->pd;
	else {
		*out = NULL;
		return E_NOINTERFACE;
	}
	p->lpVtbl->AddRef(p);
	return S_OK;
}

static ULONG WINAPI dmo_addref(IMediaObject *p)
{
	return InterlockedIncrement(&from(p)->refs);
}

static void drop_pending(struct dmo *d)
{
	if (d->pending)
		d->pending->lpVtbl->Release(d->pending);
	d->pending = NULL;
	for (DWORD i = 0; i < d->nheld; i++)
		release(d->held[i]);
	d->nheld = 0;
	d->draining = d->bursting = FALSE;
}

static ULONG WINAPI dmo_release(IMediaObject *p)
{
	struct dmo *d = from(p);
	LONG n = InterlockedDecrement(&d->refs);

	if (!n) {
		drop_pending(d);
		free_type(&d->in);
		free_type(&d->out);
		release(d->picture);
		release(d);
		InterlockedDecrement(&objects);
	}
	return n;
}

static HRESULT WINAPI dmo_stream_count(IMediaObject *p, DWORD *in, DWORD *out)
{
	if (!in || !out)
		return E_POINTER;
	*in = *out = 1;
	return S_OK;
}

static HRESULT WINAPI dmo_input_info(IMediaObject *p, DWORD stream,
				     DWORD *flags)
{
	if (stream)
		return DMO_E_INVALIDSTREAMINDEX;
	if (!flags)
		return E_POINTER;
	*flags = 0x1 | 0x2;	/* whole samples, one per buffer */
	return S_OK;
}

static HRESULT WINAPI dmo_output_info(IMediaObject *p, DWORD stream,
				      DWORD *flags)
{
	if (stream)
		return DMO_E_INVALIDSTREAMINDEX;
	if (!flags)
		return E_POINTER;
	*flags = 0x1 | 0x2;
	return S_OK;
}

/*
 * Input types: TST0 for the decoder, the raw formats for the encoder, as
 * partial types (no format block), as DMOs usually give them.
 */
static HRESULT WINAPI dmo_input_type(IMediaObject *p, DWORD stream,
				     DWORD index, DMO_MEDIA_TYPE *mt)
{
	struct dmo *d = from(p);

	if (stream)
		return DMO_E_INVALIDSTREAMINDEX;
	if (index >= (d->encoder ? NRAW : 1))
		return DMO_E_NO_MORE_ITEMS;
	if (!mt)
		return S_OK;
	for (DWORD i = 0; i < sizeof(*mt); i++)
		((BYTE *)mt)[i] = 0;
	mt->majortype = mt_video;
	if (!d->encoder)
		mt->subtype = st_tst0;
	else if (raw_formats[index].compression == BI_RGB)
		mt->subtype = raw_formats[index].bits == 24 ? st_rgb24 : st_rgb32;
	else
		mt->subtype = (GUID)FOURCC_GUID(raw_formats[index].compression);
	return S_OK;
}

/*
 * Output types, once the input is set: for the decoder each raw format of
 * the stream's depth, for the encoder TST0 of the input's depth.
 */
static HRESULT WINAPI dmo_output_type(IMediaObject *p, DWORD stream,
				      DWORD index, DMO_MEDIA_TYPE *mt)
{
	struct dmo *d = from(p);

	if (stream)
		return DMO_E_INVALIDSTREAMINDEX;
	if (!d->in_set)
		return DMO_E_TYPE_NOT_SET;
	BITMAPINFOHEADER *in = type_bih(&d->in);
	if (d->encoder) {
		if (index)
			return DMO_E_NO_MORE_ITEMS;
		return mt ? make_type(mt, in, TST0, in->biBitCount) : S_OK;
	}
	for (DWORD i = 0, n = 0; i < NRAW; i++) {
		if (raw_formats[i].bits != in->biBitCount)
			continue;
		if (n++ == index)
			return mt ? make_type(mt, in, raw_formats[i].compression,
					      raw_formats[i].bits) : S_OK;
	}
	return DMO_E_NO_MORE_ITEMS;
}

static HRESULT WINAPI dmo_set_input(IMediaObject *p, DWORD stream,
				    const DMO_MEDIA_TYPE *mt, DWORD flags)
{
	struct dmo *d = from(p);

	if (stream)
		return DMO_E_INVALIDSTREAMINDEX;
	if (flags & DMO_SET_TYPEF_CLEAR) {
		free_type(&d->in);
		d->in_set = FALSE;
		return S_OK;
	}
	if (!mt)
		return E_POINTER;
	LOWPTR(d, mt);
	LOWPTR(d, mt->pbFormat);
	BITMAPINFOHEADER *b = type_bih(mt);
	if (!eq(&mt->majortype, &mt_video) || !b || b->biWidth <= 0 ||
	    !b->biHeight)
		return DMO_E_TYPE_NOT_ACCEPTED;
	if (d->encoder ? raw_index(b) < 0 :
	    !eq(&mt->subtype, &st_tst0) || b->biCompression != TST0)
		return DMO_E_TYPE_NOT_ACCEPTED;
	if (flags & DMO_SET_TYPEF_TEST_ONLY)
		return S_OK;
	free_type(&d->in);
	free_type(&d->out);
	d->out_set = FALSE;
	HRESULT hr = copy_type(&d->in, mt);
	d->in_set = SUCCEEDED(hr);
	return hr;
}

static HRESULT WINAPI dmo_set_output(IMediaObject *p, DWORD stream,
				     const DMO_MEDIA_TYPE *mt, DWORD flags)
{
	struct dmo *d = from(p);

	if (stream)
		return DMO_E_INVALIDSTREAMINDEX;
	if (flags & DMO_SET_TYPEF_CLEAR) {
		free_type(&d->out);
		d->out_set = FALSE;
		return S_OK;
	}
	if (!mt)
		return E_POINTER;
	if (!d->in_set)
		return DMO_E_TYPE_NOT_SET;
	LOWPTR(d, mt);
	LOWPTR(d, mt->pbFormat);
	BITMAPINFOHEADER *b = type_bih(mt), *in = type_bih(&d->in);
	if (!eq(&mt->majortype, &mt_video) || !b ||
	    b->biWidth != in->biWidth || abs_long(b->biHeight) !=
	    abs_long(in->biHeight) || b->biBitCount != in->biBitCount)
		return DMO_E_TYPE_NOT_ACCEPTED;
	if (d->encoder ? b->biCompression != TST0 : raw_index(b) < 0)
		return DMO_E_TYPE_NOT_ACCEPTED;
	if (flags & DMO_SET_TYPEF_TEST_ONLY)
		return S_OK;
	free_type(&d->out);
	HRESULT hr = copy_type(&d->out, mt);
	d->out_set = SUCCEEDED(hr);
	return hr;
}

static HRESULT WINAPI dmo_input_current(IMediaObject *p, DWORD stream,
					DMO_MEDIA_TYPE *mt)
{
	struct dmo *d = from(p);

	if (stream)
		return DMO_E_INVALIDSTREAMINDEX;
	if (!mt)
		return E_POINTER;
	return d->in_set ? copy_type(mt, &d->in) : DMO_E_TYPE_NOT_SET;
}

/*
 * TESTDMO_SWITCH: from the second picture on, the output is YV12. Pictures
 * are counted as they are given out: the second is written as YV12 when
 * one has gone, and GetOutputCurrentType says YV12 once it has gone too.
 */
static BOOL switched(const struct dmo *d, DWORD given)
{
	BITMAPINFOHEADER *b = type_bih(&d->out);
	return d->switching && !d->encoder && b && b->biCompression == I420 &&
	       d->frame >= given;
}

static HRESULT WINAPI dmo_output_current(IMediaObject *p, DWORD stream,
					 DMO_MEDIA_TYPE *mt)
{
	struct dmo *d = from(p);

	if (stream)
		return DMO_E_INVALIDSTREAMINDEX;
	if (!mt)
		return E_POINTER;
	if (!d->out_set)
		return DMO_E_TYPE_NOT_SET;
	if (switched(d, 2))
		return make_type(mt, type_bih(&d->out), YV12, 12);
	return copy_type(mt, &d->out);
}

static HRESULT WINAPI dmo_input_size(IMediaObject *p, DWORD stream,
				     DWORD *size, DWORD *lookahead,
				     DWORD *align)
{
	struct dmo *d = from(p);

	if (stream)
		return DMO_E_INVALIDSTREAMINDEX;
	if (!size || !lookahead || !align)
		return E_POINTER;
	if (!d->in_set)
		return DMO_E_TYPE_NOT_SET;
	*size = d->encoder ? frame_bytes(type_bih(&d->in)) : 4;
	*lookahead = 0;
	*align = 1;
	return S_OK;
}

static HRESULT WINAPI dmo_output_size(IMediaObject *p, DWORD stream,
				      DWORD *size, DWORD *align)
{
	struct dmo *d = from(p);

	if (stream)
		return DMO_E_INVALIDSTREAMINDEX;
	if (!size || !align)
		return E_POINTER;
	if (!d->out_set)
		return DMO_E_TYPE_NOT_SET;
	DWORD raw = frame_bytes(type_bih(d->encoder ? &d->in : &d->out));
	*size = d->encoder ? raw * 2 + 4096 : raw;
	*align = 1;
	return S_OK;
}

static HRESULT WINAPI dmo_get_latency(IMediaObject *p, DWORD s,
				      REFERENCE_TIME *t)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI dmo_set_latency(IMediaObject *p, DWORD s,
				      REFERENCE_TIME t)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI dmo_flush(IMediaObject *p)
{
	struct dmo *d = from(p);

	drop_pending(d);
	d->valid = FALSE;
	return S_OK;
}

static HRESULT WINAPI dmo_discontinuity(IMediaObject *p, DWORD stream)
{
	if (stream)
		return DMO_E_INVALIDSTREAMINDEX;
	from(p)->draining = TRUE;
	return S_OK;
}

static HRESULT WINAPI dmo_allocate(IMediaObject *p)
{
	return from(p)->in_set && from(p)->out_set ? S_OK : DMO_E_TYPE_NOT_SET;
}

static HRESULT WINAPI dmo_free_resources(IMediaObject *p)
{
	drop_pending(from(p));
	return S_OK;
}

static HRESULT WINAPI dmo_input_status(IMediaObject *p, DWORD stream,
				       DWORD *flags)
{
	if (stream)
		return DMO_E_INVALIDSTREAMINDEX;
	if (!flags)
		return E_POINTER;
	*flags = from(p)->pending ? 0 : DMO_INPUT_STATUSF_ACCEPT_DATA;
	return S_OK;
}

static HRESULT WINAPI dmo_process_input(IMediaObject *p, DWORD stream,
					IMediaBuffer *buffer, DWORD flags,
					REFERENCE_TIME time,
					REFERENCE_TIME length)
{
	struct dmo *d = from(p);

	if (stream)
		return DMO_E_INVALIDSTREAMINDEX;
	if (!buffer)
		return E_POINTER;
	LOWPTR(d, buffer);
	if (!d->in_set || !d->out_set)
		return DMO_E_TYPE_NOT_SET;
	if (d->pending || (d->refuse && d->inputs++ % 2 == 0))
		return DMO_E_NOTACCEPTING;
	buffer->lpVtbl->AddRef(buffer);
	d->pending = buffer;
	d->pending_flags = flags;
	d->pending_time = time;
	d->pending_length = length;
	d->draining = FALSE;
	return S_OK;
}

/* Encodes the pending input into dst (cap bytes); its length, or 0. */
static HRESULT encode(struct dmo *d, const BYTE *src, DWORD len, BYTE *dst,
		      DWORD cap, DWORD *out, BOOL *key)
{
	BITMAPINFOHEADER *in = type_bih(&d->in);
	DWORD raw = frame_bytes(in);

	if (len < raw)
		return E_INVALIDARG;
	*key = (d->pending_flags & DMO_INPUT_DATA_BUFFERF_SYNCPOINT) ||
	       !d->valid || (d->key_interval > 0 &&
			     d->frame % (DWORD)d->key_interval == 0);
	if (cap < raw * 2 + 4)
		return E_INVALIDARG;
	dst[0] = *key ? 'K' : 'D';
	dst[1] = (BYTE)in->biBitCount;
	dst[2] = dst[3] = 0;
	if (*key) {
		*out = 4 + rle_encode(src, raw, dst + 4);
	} else {
		for (DWORD i = 0; i < raw; i++)
			d->picture[i] ^= src[i];
		*out = 4 + rle_encode(d->picture, raw, dst + 4);
	}
	copy(d->picture, src, raw);
	d->valid = TRUE;
	return S_OK;
}

/* Decodes the pending input into the current picture. */
static HRESULT decode(struct dmo *d, const BYTE *src, DWORD len)
{
	BITMAPINFOHEADER *in = type_bih(&d->in);

	if (len < 4 || src[1] != in->biBitCount || (src[0] != 'K' && src[0] != 'D'))
		return E_INVALIDARG;
	if (src[0] == 'D' && !d->valid)
		return S_FALSE;
	if (rle_decode(src + 4, len - 4, d->picture, d->picture_size,
		       src[0] == 'D'))
		return E_INVALIDARG;
	d->valid = TRUE;
	return S_OK;
}

/* A decoded picture into an output buffer, as YV12 if switched. */
static HRESULT put_picture(struct dmo *d, const BYTE *picture,
			   DMO_OUTPUT_DATA_BUFFER *ob)
{
	BYTE *dst = NULL;
	DWORD max = 0, raw = d->picture_size;

	if (!ob->pBuffer)
		return E_POINTER;
	LOWPTR(d, ob->pBuffer);
	if (FAILED(ob->pBuffer->lpVtbl->GetBufferAndLength(ob->pBuffer, &dst,
							   NULL)) ||
	    FAILED(ob->pBuffer->lpVtbl->GetMaxLength(ob->pBuffer, &max)))
		return E_FAIL;
	LOWPTR(d, dst);
	if (max < raw)
		return E_INVALIDARG;
	copy(dst, picture, raw);
	if (switched(d, 1)) {
		BITMAPINFOHEADER *b = type_bih(&d->out);
		DWORD y = (DWORD)b->biWidth * abs_long(b->biHeight);
		DWORD c = (raw - y) / 2;
		copy(dst + y, picture + y + c, c);
		copy(dst + y + c, picture + y, c);
	}
	return ob->pBuffer->lpVtbl->SetLength(ob->pBuffer, raw);
}

/*
 * The oldest held picture, unstamped; INCOMPLETE if more of a burst, or of
 * what end of stream lets out, follows.
 */
static HRESULT give_held(struct dmo *d, DMO_OUTPUT_DATA_BUFFER *ob)
{
	HRESULT hr = put_picture(d, d->held[0], ob);

	if (FAILED(hr))
		return hr;
	release(d->held[0]);
	d->held[0] = d->held[1];
	d->held[1] = NULL;
	d->nheld--;
	d->frame++;
	if (!d->nheld)
		d->bursting = FALSE;
	ob->dwStatus = DMO_OUTPUT_DATA_BUFFERF_SYNCPOINT;
	if (d->nheld && (d->bursting || d->draining))
		ob->dwStatus |= DMO_OUTPUT_DATA_BUFFERF_INCOMPLETE;
	return S_OK;
}

static HRESULT WINAPI dmo_process_output(IMediaObject *p, DWORD flags,
					 DWORD count,
					 DMO_OUTPUT_DATA_BUFFER *buffers,
					 DWORD *status)
{
	struct dmo *d = from(p);
	BYTE *src = NULL, *dst = NULL;
	DWORD len = 0, max = 0;
	HRESULT hr;

	if (!buffers || !status || count != 1)
		return E_POINTER;
	LOWPTR(d, buffers);
	*status = 0;
	buffers[0].dwStatus = 0;
	if (!d->out_set)
		return DMO_E_TYPE_NOT_SET;
	DWORD raw = frame_bytes(type_bih(d->encoder ? &d->in : &d->out));
	if (d->picture_size != raw) {
		release(d->picture);
		if (!(d->picture = alloc(raw)))
			return E_OUTOFMEMORY;
		d->picture_size = raw;
		d->valid = FALSE;
	}

	/* What DELAY and BURST held back comes out first. */
	if (!d->pending) {
		if (!d->nheld || (!d->draining && !d->bursting))
			return S_FALSE;
		return give_held(d, &buffers[0]);
	}

	if (FAILED(hr = d->pending->lpVtbl->GetBufferAndLength(d->pending, &src,
							       &len)))
		return hr;
	LOWPTR(d, src);
	IMediaBuffer *in = d->pending;
	d->pending = NULL;

	if (d->encoder) {
		BOOL key = FALSE;
		DWORD n = 0;
		if (!buffers[0].pBuffer) {
			in->lpVtbl->Release(in);
			return E_POINTER;
		}
		LOWPTR(d, buffers[0].pBuffer);
		buffers[0].pBuffer->lpVtbl->GetBufferAndLength(buffers[0].pBuffer,
							       &dst, NULL);
		buffers[0].pBuffer->lpVtbl->GetMaxLength(buffers[0].pBuffer, &max);
		hr = encode(d, src, len, dst, max, &n, &key);
		in->lpVtbl->Release(in);
		if (FAILED(hr))
			return hr;
		buffers[0].pBuffer->lpVtbl->SetLength(buffers[0].pBuffer, n);
		buffers[0].dwStatus = key ? DMO_OUTPUT_DATA_BUFFERF_SYNCPOINT : 0;
	} else {
		hr = decode(d, src, len);
		in->lpVtbl->Release(in);
		if (FAILED(hr))
			return hr;
		if (hr == S_FALSE)
			return S_FALSE;		/* a difference with no key yet */
		if (d->delay || d->burst) {
			BYTE *copy_of = alloc(raw);
			if (!copy_of)
				return E_OUTOFMEMORY;
			copy(copy_of, d->picture, raw);
			d->held[d->nheld++] = copy_of;
			/* DELAY: one held back; BURST: two at a time. */
			if (d->nheld < 2)
				return S_FALSE;
			d->bursting = d->burst;
			return give_held(d, &buffers[0]);
		}
		if (FAILED(hr = put_picture(d, d->picture, &buffers[0])))
			return hr;
		buffers[0].dwStatus = DMO_OUTPUT_DATA_BUFFERF_SYNCPOINT;
	}
	d->frame++;
	if (d->pending_flags & DMO_INPUT_DATA_BUFFERF_TIME) {
		buffers[0].dwStatus |= DMO_OUTPUT_DATA_BUFFERF_TIME;
		buffers[0].rtTimestamp = d->pending_time;
	}
	if (d->pending_flags & DMO_INPUT_DATA_BUFFERF_TIMELENGTH) {
		buffers[0].dwStatus |= DMO_OUTPUT_DATA_BUFFERF_TIMELENGTH;
		buffers[0].rtTimelength = d->pending_length;
	}
	return S_OK;
}

static HRESULT WINAPI dmo_lock(IMediaObject *p, LONG lock)
{
	return S_OK;
}

static const IMediaObjectVtbl dmo_vtbl = {
	dmo_qi, dmo_addref, dmo_release, dmo_stream_count, dmo_input_info,
	dmo_output_info, dmo_input_type, dmo_output_type, dmo_set_input,
	dmo_set_output, dmo_input_current, dmo_output_current, dmo_input_size,
	dmo_output_size, dmo_get_latency, dmo_set_latency, dmo_flush,
	dmo_discontinuity, dmo_allocate, dmo_free_resources, dmo_input_status,
	dmo_process_input, dmo_process_output, dmo_lock,
};

/* IPersistStream: the encoder's key interval. */

#define FROM(type, member, p) \
	((struct dmo *)((BYTE *)(p) - FIELD_OFFSET(struct dmo, member)))

struct saved { DWORD magic; LONG key_interval; };

static HRESULT WINAPI persist_qi(IPersistStream *p, REFIID i, void **o)
{
	return dmo_qi(&FROM(IPersistStream, persist, p)->iface, i, o);
}
static ULONG WINAPI persist_addref(IPersistStream *p)
{
	return dmo_addref(&FROM(IPersistStream, persist, p)->iface);
}
static ULONG WINAPI persist_release(IPersistStream *p)
{
	return dmo_release(&FROM(IPersistStream, persist, p)->iface);
}
static HRESULT WINAPI persist_class(IPersistStream *p, CLSID *c)
{
	return E_NOTIMPL;
}
static HRESULT WINAPI persist_dirty(IPersistStream *p)
{
	return S_FALSE;
}
static HRESULT WINAPI persist_load(IPersistStream *p, IStream *s)
{
	struct saved x;
	ULONG got = 0;

	if (!s)
		return E_POINTER;
	HRESULT hr = s->lpVtbl->Read(s, &x, sizeof(x), &got);
	if (FAILED(hr))
		return hr;
	if (got != sizeof(x) || x.magic != TST0)
		return E_FAIL;
	FROM(IPersistStream, persist, p)->key_interval = x.key_interval;
	return S_OK;
}
static HRESULT WINAPI persist_save(IPersistStream *p, IStream *s, BOOL clear)
{
	struct saved x = { TST0, FROM(IPersistStream, persist, p)->key_interval };
	ULONG wrote = 0;

	if (!s)
		return E_POINTER;
	HRESULT hr = s->lpVtbl->Write(s, &x, sizeof(x), &wrote);
	return SUCCEEDED(hr) && wrote != sizeof(x) ? E_FAIL : hr;
}
static HRESULT WINAPI persist_size(IPersistStream *p, ULARGE_INTEGER *n)
{
	if (!n)
		return E_POINTER;
	n->QuadPart = sizeof(struct saved);
	return S_OK;
}
static IPersistStreamVtbl persist_vtbl = {
	persist_qi, persist_addref, persist_release, persist_class,
	persist_dirty, persist_load, persist_save, persist_size,
};

/* ICodecAPI: the encoder's GOP size, as an unsigned 32-bit value. */

static HRESULT WINAPI api_qi(ICodecAPI *p, REFIID i, void **o)
{
	return dmo_qi(&FROM(ICodecAPI, api, p)->iface, i, o);
}
static ULONG WINAPI api_addref(ICodecAPI *p)
{
	return dmo_addref(&FROM(ICodecAPI, api, p)->iface);
}
static ULONG WINAPI api_release(ICodecAPI *p)
{
	return dmo_release(&FROM(ICodecAPI, api, p)->iface);
}
static HRESULT WINAPI api_supported(ICodecAPI *p, const GUID *a)
{
	return a && eq(a, &api_gop_size) ? S_OK : S_FALSE;
}
static HRESULT WINAPI api_modifiable(ICodecAPI *p, const GUID *a)
{
	return api_supported(p, a);
}
static HRESULT WINAPI api_range(ICodecAPI *p, const GUID *a, VARIANT *x,
				VARIANT *y, VARIANT *z)
{
	return E_NOTIMPL;
}
static HRESULT WINAPI api_values(ICodecAPI *p, const GUID *a, VARIANT **v,
				 ULONG *n)
{
	return E_NOTIMPL;
}
static HRESULT WINAPI api_get(ICodecAPI *p, const GUID *a, VARIANT *v)
{
	if (!a || !v)
		return E_POINTER;
	if (!eq(a, &api_gop_size))
		return E_NOTIMPL;
	v->vt = VT_UI4;
	v->ulVal = (ULONG)FROM(ICodecAPI, api, p)->key_interval;
	return S_OK;
}
static HRESULT WINAPI api_set(ICodecAPI *p, const GUID *a, VARIANT *v)
{
	if (!a || !v)
		return E_POINTER;
	if (!eq(a, &api_gop_size) || v->vt != VT_UI4)
		return E_INVALIDARG;
	FROM(ICodecAPI, api, p)->key_interval = (LONG)v->ulVal;
	return S_OK;
}
static HRESULT WINAPI api_event(ICodecAPI *p, const GUID *a, LONG_PTR u)
{
	return E_NOTIMPL;
}
static HRESULT WINAPI api_unevent(ICodecAPI *p, const GUID *a)
{
	return E_NOTIMPL;
}
static HRESULT WINAPI api_defaults(ICodecAPI *p)
{
	FROM(ICodecAPI, api, p)->key_interval = 0;
	return S_OK;
}
static HRESULT WINAPI api_set_notify(ICodecAPI *p, const GUID *a, VARIANT *v,
				     GUID **c, ULONG *n)
{
	return E_NOTIMPL;
}
static HRESULT WINAPI api_defaults_notify(ICodecAPI *p, GUID **c, ULONG *n)
{
	return E_NOTIMPL;
}
static HRESULT WINAPI api_all(ICodecAPI *p, IStream *s)
{
	return E_NOTIMPL;
}
static HRESULT WINAPI api_all_notify(ICodecAPI *p, IStream *s, GUID **c,
				     ULONG *n)
{
	return E_NOTIMPL;
}
static ICodecAPIVtbl api_vtbl = {
	api_qi, api_addref, api_release, api_supported, api_modifiable,
	api_range, api_values, api_get, api_get, api_set, api_event,
	api_unevent, api_defaults, api_set_notify, api_defaults_notify,
	api_all, api_all, api_all_notify,
};

/* IWMCodecPrivateData (TESTDMO_PRIVATE): four bytes, "TSTP". */

static HRESULT WINAPI pd_qi(IWMCodecPrivateData *p, REFIID i, void **o)
{
	return dmo_qi(&FROM(IWMCodecPrivateData, pd, p)->iface, i, o);
}
static ULONG WINAPI pd_addref(IWMCodecPrivateData *p)
{
	return dmo_addref(&FROM(IWMCodecPrivateData, pd, p)->iface);
}
static ULONG WINAPI pd_release(IWMCodecPrivateData *p)
{
	return dmo_release(&FROM(IWMCodecPrivateData, pd, p)->iface);
}
static HRESULT WINAPI pd_partial(IWMCodecPrivateData *p, DMO_MEDIA_TYPE *mt)
{
	struct dmo *d = FROM(IWMCodecPrivateData, pd, p);

	if (!mt)
		return E_POINTER;
	if (!d->in_set)
		return DMO_E_TYPE_NOT_SET;
	return eq(&mt->subtype, &st_tst0) ? S_OK : DMO_E_TYPE_NOT_ACCEPTED;
}
static HRESULT WINAPI pd_data(IWMCodecPrivateData *p, BYTE *data, DWORD *size)
{
	if (!size)
		return E_POINTER;
	if (data) {
		if (*size < 4)
			return E_INVALIDARG;
		data[0] = 'T';
		data[1] = 'S';
		data[2] = 'T';
		data[3] = 'P';
	}
	*size = 4;
	return S_OK;
}
static const IWMCodecPrivateDataVtbl pd_vtbl = {
	pd_qi, pd_addref, pd_release, pd_partial, pd_data,
};

HRESULT testdmo_create(BOOL encoder, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	struct dmo *d = alloc(sizeof(*d));
	if (!d)
		return E_OUTOFMEMORY;
	d->iface.lpVtbl = (IMediaObjectVtbl *)&dmo_vtbl;
	d->persist.lpVtbl = &persist_vtbl;
	d->api.lpVtbl = &api_vtbl;
	d->pd.lpVtbl = &pd_vtbl;
	d->refs = 1;
	d->encoder = encoder;
	d->delay = env_on("TESTDMO_DELAY");
	d->burst = env_on("TESTDMO_BURST");
	d->refuse = env_on("TESTDMO_NOTACCEPTING");
	d->switching = env_on("TESTDMO_SWITCH");
	d->private_data = env_on("TESTDMO_PRIVATE");
	d->strict = env_on("TESTDMO_STRICT");
	InterlockedIncrement(&objects);
	HRESULT hr = dmo_qi(&d->iface, iid, out);
	dmo_release(&d->iface);
	return hr;
}

LONG testdmo_objects(void)
{
	return objects;
}
