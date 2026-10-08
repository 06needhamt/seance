/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * testdmo.dll's audio DMOs: a TSTA decoder and encoder (tsta.h), CRT-less,
 * so testfilter.ax's audio filters and these can be cross-checked byte for
 * byte.
 *
 * Like most DMOs, ProcessInput keeps the buffer it is given and
 * ProcessOutput turns it into output. The encoder gathers PCM into whole
 * packets, giving one per ProcessOutput (INCOMPLETE while the input holds
 * another) and the last, short one after Discontinuity. Modes, from
 * environment variables set to 1, as for the video DMOs:
 *   TESTDMO_AUDIO_SPLIT  the decoder gives its PCM in pieces of 300 sample
 *                        frames, each but the last INCOMPLETE
 *   TESTDMO_AUDIO_HOLD   the decoder keeps each packet's PCM until the next
 *                        packet comes, the last until Discontinuity
 *   TESTDMO_AUDIO_FLOAT  from the second packet on, the decoder gives
 *                        32-bit float, as GetOutputCurrentType then says
 *   TESTDMO_AUDIO_RATE   from the second packet on, GetOutputCurrentType
 *                        claims twice the sample rate
 *   TESTDMO_NOTACCEPTING every other ProcessInput is refused
 *   TESTDMO_STRICT       every pointer handed in must be below 2 GB
 */
#include "fixture.h"
#include <dmoreg.h>
#include <mediaerr.h>
#include <strmif.h>
#include <mmreg.h>
#include "tsta.h"

static const GUID iid_unknown = { 0, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID iid_media_object = { 0xd8ad0f58, 0x5494, 0x4102,
	{ 0x97, 0xc5, 0xec, 0x79, 0x8e, 0x59, 0xbc, 0xf4 } };
static const GUID iid_codec_api = { 0x901db4c7, 0x31ce, 0x41a2,
	{ 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda } };
static const GUID api_mean_bitrate = { 0xf7222374, 0x2144, 0x4815,
	{ 0xb5, 0x50, 0xa3, 0x7f, 0x8e, 0x12, 0xee, 0x52 } };
#define TAG_GUID(t) { (t), 0x0000, 0x0010, \
	{ 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } }
static const GUID mt_audio = TAG_GUID(0x73647561);	/* 'auds' */
static const GUID st_pcm = TAG_GUID(WAVE_FORMAT_PCM);
static const GUID st_float = TAG_GUID(WAVE_FORMAT_IEEE_FLOAT);
static const GUID st_tsta = TAG_GUID(WAVE_FORMAT_TSTA);
static const GUID format_wave = { 0x05589f81, 0xc356, 0x11ce,
	{ 0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a } };

#define PIECE 300

struct admo {
	IMediaObject iface;
	ICodecAPI api;
	LONG refs;
	BOOL encoder;
	BOOL split, hold, to_float, rate, refuse, strict;
	DMO_MEDIA_TYPE in, out;
	BOOL in_set, out_set;
	DWORD bitrate, inputs, packets;
	/* the input held */
	IMediaBuffer *pending;
	DWORD pending_used;		/* bytes taken from it (encoder) */
	BOOL pending_timed, draining, switched;
	REFERENCE_TIME pending_time;
	/* the encoder: PCM gathered, and the frames packed before it */
	BYTE *gather;
	DWORD gathered;
	BOOL based;
	REFERENCE_TIME base;
	DWORDLONG frames_done;
	/* the decoder: PCM being given out, and one packet held back (HOLD) */
	BYTE *ready, *held, *spare;
	DWORD ready_frames, ready_pos, ready_packet, held_frames, held_packet;
	BOOL have_held, ready_timed, held_timed;
	REFERENCE_TIME ready_time, held_time;
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

static void zero(void *p, DWORD n)
{
	for (DWORD i = 0; i < n; i++)
		((BYTE *)p)[i] = 0;
}

static void copy(void *d, const void *s, DWORD n)
{
	for (DWORD i = 0; i < n; i++)
		((BYTE *)d)[i] = ((const BYTE *)s)[i];
}

static BOOL high(const struct admo *d, const void *p)
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
	zero(m, sizeof(*m));
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

static WAVEFORMATEX *type_wfx(const DMO_MEDIA_TYPE *mt)
{
	if (!mt || !eq(&mt->majortype, &mt_audio) ||
	    !eq(&mt->formattype, &format_wave) || !mt->pbFormat ||
	    mt->cbFormat < sizeof(WAVEFORMATEX))
		return NULL;
	return (WAVEFORMATEX *)mt->pbFormat;
}

/* Integer PCM, plain or extensible: its bits per sample, else 0. */
static DWORD pcm_bits(const DMO_MEDIA_TYPE *mt)
{
	WAVEFORMATEX *wf = type_wfx(mt);

	if (!wf || !eq(&mt->subtype, &st_pcm))
		return 0;
	if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
		WAVEFORMATEXTENSIBLE *x = (void *)wf;
		if (mt->cbFormat < sizeof(*x) || wf->cbSize < 22 ||
		    !eq(&x->SubFormat, &st_pcm) ||
		    x->Samples.wValidBitsPerSample != wf->wBitsPerSample)
			return 0;
	} else if (wf->wFormatTag != WAVE_FORMAT_PCM) {
		return 0;
	}
	if (!wf->nSamplesPerSec ||
	    !tsta_pcm_block(wf->wBitsPerSample, wf->nChannels) ||
	    wf->nBlockAlign != tsta_pcm_block(wf->wBitsPerSample, wf->nChannels))
		return 0;
	return wf->wBitsPerSample;
}

static WAVEFORMATEX *tsta_type(const DMO_MEDIA_TYPE *mt)
{
	WAVEFORMATEX *wf = type_wfx(mt);

	if (!wf || !eq(&mt->subtype, &st_tsta) ||
	    wf->wFormatTag != WAVE_FORMAT_TSTA || !wf->nSamplesPerSec ||
	    !tsta_pcm_block(wf->wBitsPerSample, wf->nChannels))
		return NULL;
	return wf;
}

/* A whole type with a WAVEFORMATEX of size bytes, to fill in. */
static WAVEFORMATEX *new_type(DMO_MEDIA_TYPE *mt, const GUID *subtype,
			      DWORD size)
{
	zero(mt, sizeof(*mt));
	if (!(mt->pbFormat = CoTaskMemAlloc(size)))
		return NULL;
	zero(mt->pbFormat, size);
	mt->majortype = mt_audio;
	mt->subtype = *subtype;
	mt->formattype = format_wave;
	mt->cbFormat = size;
	return (WAVEFORMATEX *)mt->pbFormat;
}

/* PCM (or float) of these bits, channels and rate; extensible or plain. */
static HRESULT pcm_type(DMO_MEDIA_TYPE *mt, BOOL is_float, WORD bits,
			WORD channels, DWORD rate, BOOL extensible)
{
	const GUID *sub = is_float ? &st_float : &st_pcm;
	WAVEFORMATEX *wf = new_type(mt, sub, extensible ?
				    sizeof(WAVEFORMATEXTENSIBLE) :
				    sizeof(WAVEFORMATEX));

	if (!wf)
		return E_OUTOFMEMORY;
	wf->wFormatTag = is_float ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
	wf->nChannels = channels;
	wf->nSamplesPerSec = rate;
	wf->wBitsPerSample = bits;
	wf->nBlockAlign = (WORD)(bits / 8 * channels);
	wf->nAvgBytesPerSec = rate * wf->nBlockAlign;
	if (extensible) {
		WAVEFORMATEXTENSIBLE *x = (void *)wf;
		wf->wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		wf->cbSize = 22;
		x->Samples.wValidBitsPerSample = bits;
		x->dwChannelMask = channels == 2 ? 3 : 0;
		x->SubFormat = *sub;
	}
	mt->bFixedSizeSamples = TRUE;
	mt->lSampleSize = wf->nBlockAlign;
	return S_OK;
}

/* The encoder's output: TSTA of its input's PCM. */
static HRESULT made_type(struct admo *d, DMO_MEDIA_TYPE *mt)
{
	WAVEFORMATEX *in = type_wfx(&d->in);
	WAVEFORMATEX *wf = new_type(mt, &st_tsta, sizeof(WAVEFORMATEX) + 2);

	if (!wf)
		return E_OUTOFMEMORY;
	tsta_format(wf, in->wBitsPerSample, in->nChannels, in->nSamplesPerSec,
		    d->bitrate);
	return S_OK;
}

/* Bytes of a PCM sample frame of the input (encoder) or stream. */
static DWORD block_of(const struct admo *d)
{
	WAVEFORMATEX *in = type_wfx(&d->in);
	return tsta_pcm_block(in->wBitsPerSample, in->nChannels);
}

/* IMediaObject. */

static struct admo *from(IMediaObject *p)
{
	return (struct admo *)p;
}

static HRESULT WINAPI dmo_qi(IMediaObject *p, REFIID iid, void **out)
{
	struct admo *d = from(p);

	if (!out)
		return E_POINTER;
	if (eq(iid, &iid_unknown) || eq(iid, &iid_media_object))
		*out = p;
	else if (d->encoder && eq(iid, &iid_codec_api))
		*out = &d->api;
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

/* Forgets the stream so far (a flush, or new types). */
static void reset(struct admo *d)
{
	if (d->pending)
		d->pending->lpVtbl->Release(d->pending);
	d->pending = NULL;
	d->pending_used = 0;
	d->draining = d->switched = d->based = d->have_held = FALSE;
	d->gathered = d->ready_frames = d->ready_pos = 0;
	d->frames_done = 0;
	d->packets = 0;
}

static void free_buffers(struct admo *d)
{
	release(d->gather);
	release(d->ready);
	release(d->held);
	release(d->spare);
	d->gather = d->ready = d->held = d->spare = NULL;
}

static ULONG WINAPI dmo_release(IMediaObject *p)
{
	struct admo *d = from(p);
	LONG n = InterlockedDecrement(&d->refs);

	if (!n) {
		reset(d);
		free_buffers(d);
		free_type(&d->in);
		free_type(&d->out);
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
	*flags = 0;
	return S_OK;
}

static HRESULT WINAPI dmo_output_info(IMediaObject *p, DWORD stream,
				      DWORD *flags)
{
	return dmo_input_info(p, stream, flags);
}

/* Input types: TSTA for the decoder, PCM for the encoder, partial. */
static HRESULT WINAPI dmo_input_type(IMediaObject *p, DWORD stream,
				     DWORD index, DMO_MEDIA_TYPE *mt)
{
	struct admo *d = from(p);

	if (stream)
		return DMO_E_INVALIDSTREAMINDEX;
	if (index)
		return DMO_E_NO_MORE_ITEMS;
	if (!mt)
		return S_OK;
	zero(mt, sizeof(*mt));
	mt->majortype = mt_audio;
	mt->subtype = d->encoder ? st_pcm : st_tsta;
	return S_OK;
}

/*
 * Output types, once the input is set: the decoder's PCM of the stream's
 * bits, plain then extensible; the encoder's TSTA.
 */
static HRESULT WINAPI dmo_output_type(IMediaObject *p, DWORD stream,
				      DWORD index, DMO_MEDIA_TYPE *mt)
{
	struct admo *d = from(p);

	if (stream)
		return DMO_E_INVALIDSTREAMINDEX;
	if (!d->in_set)
		return DMO_E_TYPE_NOT_SET;
	if (index >= (d->encoder ? 1u : 2u))
		return DMO_E_NO_MORE_ITEMS;
	if (!mt)
		return S_OK;
	if (d->encoder)
		return made_type(d, mt);
	WAVEFORMATEX *in = type_wfx(&d->in);
	return pcm_type(mt, FALSE, in->wBitsPerSample, in->nChannels,
			in->nSamplesPerSec, index == 1);
}

static HRESULT WINAPI dmo_set_input(IMediaObject *p, DWORD stream,
				    const DMO_MEDIA_TYPE *mt, DWORD flags)
{
	struct admo *d = from(p);

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
	if (d->encoder ? !pcm_bits(mt) : !tsta_type(mt))
		return DMO_E_TYPE_NOT_ACCEPTED;
	if (flags & DMO_SET_TYPEF_TEST_ONLY)
		return S_OK;
	free_type(&d->in);
	free_type(&d->out);
	d->out_set = FALSE;
	reset(d);
	free_buffers(d);
	HRESULT hr = copy_type(&d->in, mt);
	d->in_set = SUCCEEDED(hr);
	return hr;
}

static HRESULT WINAPI dmo_set_output(IMediaObject *p, DWORD stream,
				     const DMO_MEDIA_TYPE *mt, DWORD flags)
{
	struct admo *d = from(p);

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
	WAVEFORMATEX *in = type_wfx(&d->in);
	WAVEFORMATEX *out = d->encoder ? tsta_type(mt) : type_wfx(mt);
	if (!out || out->nChannels != in->nChannels ||
	    out->nSamplesPerSec != in->nSamplesPerSec ||
	    (d->encoder ? out->wBitsPerSample != in->wBitsPerSample :
	     pcm_bits(mt) != in->wBitsPerSample))
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
	struct admo *d = from(p);

	if (stream)
		return DMO_E_INVALIDSTREAMINDEX;
	if (!mt)
		return E_POINTER;
	return d->in_set ? copy_type(mt, &d->in) : DMO_E_TYPE_NOT_SET;
}

/* After the switch (FLOAT, RATE): float, or the rate claimed doubled. */
static HRESULT WINAPI dmo_output_current(IMediaObject *p, DWORD stream,
					 DMO_MEDIA_TYPE *mt)
{
	struct admo *d = from(p);

	if (stream)
		return DMO_E_INVALIDSTREAMINDEX;
	if (!mt)
		return E_POINTER;
	if (!d->out_set)
		return DMO_E_TYPE_NOT_SET;
	if (d->switched && d->to_float) {
		WAVEFORMATEX *wf = type_wfx(&d->out);
		return pcm_type(mt, TRUE, 32, wf->nChannels, wf->nSamplesPerSec,
				TRUE);
	}
	HRESULT hr = copy_type(mt, &d->out);
	if (SUCCEEDED(hr) && d->switched && d->rate) {
		WAVEFORMATEX *wf = (WAVEFORMATEX *)mt->pbFormat;
		wf->nSamplesPerSec *= 2;
		wf->nAvgBytesPerSec *= 2;
	}
	return hr;
}

static HRESULT WINAPI dmo_input_size(IMediaObject *p, DWORD stream,
				     DWORD *size, DWORD *lookahead,
				     DWORD *align)
{
	struct admo *d = from(p);

	if (stream)
		return DMO_E_INVALIDSTREAMINDEX;
	if (!size || !lookahead || !align)
		return E_POINTER;
	if (!d->in_set)
		return DMO_E_TYPE_NOT_SET;
	*size = d->encoder ? block_of(d) : 1;
	*lookahead = 0;
	*align = 1;
	return S_OK;
}

static HRESULT WINAPI dmo_output_size(IMediaObject *p, DWORD stream,
				      DWORD *size, DWORD *align)
{
	struct admo *d = from(p);

	if (stream)
		return DMO_E_INVALIDSTREAMINDEX;
	if (!size || !align)
		return E_POINTER;
	if (!d->out_set)
		return DMO_E_TYPE_NOT_SET;
	/* A whole packet, or a packet's PCM as float. */
	*size = d->encoder ? TSTA_HEADER + TSTA_FRAMES * block_of(d) :
		TSTA_FRAMES * 4 * type_wfx(&d->in)->nChannels;
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
	reset(from(p));
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
	struct admo *d = from(p);
	DWORD packet;

	if (!d->in_set || !d->out_set)
		return DMO_E_TYPE_NOT_SET;
	packet = TSTA_FRAMES * block_of(d);
	if (!d->gather && d->encoder && !(d->gather = alloc(packet)))
		return E_OUTOFMEMORY;
	if (!d->encoder && !d->ready &&
	    (!(d->ready = alloc(packet)) || !(d->held = alloc(packet)) ||
	     !(d->spare = alloc(packet)))) {
		free_buffers(d);
		return E_OUTOFMEMORY;
	}
	return S_OK;
}

static HRESULT WINAPI dmo_free_resources(IMediaObject *p)
{
	struct admo *d = from(p);

	reset(d);
	free_buffers(d);
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
	struct admo *d = from(p);

	if (stream)
		return DMO_E_INVALIDSTREAMINDEX;
	if (!buffer)
		return E_POINTER;
	LOWPTR(d, buffer);
	if (!d->in_set || !d->out_set)
		return DMO_E_TYPE_NOT_SET;
	if ((d->encoder ? !d->gather : !d->ready) &&
	    FAILED(dmo_allocate(p)))
		return E_OUTOFMEMORY;
	if (d->pending || d->ready_pos < d->ready_frames ||
	    (d->refuse && d->inputs++ % 2 == 0))
		return DMO_E_NOTACCEPTING;
	buffer->lpVtbl->AddRef(buffer);
	d->pending = buffer;
	d->pending_used = 0;
	d->pending_timed = !!(flags & DMO_INPUT_DATA_BUFFERF_TIME);
	d->pending_time = time;
	d->draining = FALSE;
	return S_OK;
}

/* The output buffer's memory, at least need bytes of it. */
static HRESULT output_memory(struct admo *d, DMO_OUTPUT_DATA_BUFFER *ob,
			     DWORD need, BYTE **dst)
{
	DWORD max = 0;

	if (!ob->pBuffer)
		return E_POINTER;
	LOWPTR(d, ob->pBuffer);
	if (FAILED(ob->pBuffer->lpVtbl->GetBufferAndLength(ob->pBuffer, dst,
							   NULL)) ||
	    FAILED(ob->pBuffer->lpVtbl->GetMaxLength(ob->pBuffer, &max)))
		return E_FAIL;
	LOWPTR(d, *dst);
	return max < need ? E_INVALIDARG : S_OK;
}

static void stamp(DMO_OUTPUT_DATA_BUFFER *ob, REFERENCE_TIME start,
		  REFERENCE_TIME stop)
{
	ob->dwStatus |= DMO_OUTPUT_DATA_BUFFERF_SYNCPOINT |
			DMO_OUTPUT_DATA_BUFFERF_TIME |
			DMO_OUTPUT_DATA_BUFFERF_TIMELENGTH;
	ob->rtTimestamp = start;
	ob->rtTimelength = stop - start;
}

/* The encoder: the PCM gathered, as a packet. */
static HRESULT emit_packet(struct admo *d, DMO_OUTPUT_DATA_BUFFER *ob)
{
	WAVEFORMATEX *in = type_wfx(&d->in);
	DWORD block = block_of(d);
	BYTE *dst;
	HRESULT hr = output_memory(d, ob, TSTA_HEADER + d->gathered * block,
				   &dst);

	if (FAILED(hr))
		return hr;
	DWORD n = tsta_encode(d->gather, d->gathered, in->wBitsPerSample,
			      in->nChannels, dst);
	ob->pBuffer->lpVtbl->SetLength(ob->pBuffer, n);
	stamp(ob, d->base + (REFERENCE_TIME)tsta_ticks(d->frames_done,
						       in->nSamplesPerSec),
	      d->base + (REFERENCE_TIME)tsta_ticks(d->frames_done + d->gathered,
						   in->nSamplesPerSec));
	d->frames_done += d->gathered;
	d->gathered = 0;
	return S_OK;
}

static HRESULT encode_output(struct admo *d, DMO_OUTPUT_DATA_BUFFER *ob)
{
	DWORD block = block_of(d);

	if (d->pending) {
		BYTE *src = NULL;
		DWORD len = 0;
		HRESULT hr = d->pending->lpVtbl->GetBufferAndLength(d->pending,
								    &src, &len);
		if (FAILED(hr))
			return hr;
		LOWPTR(d, src);
		if (len % block)
			return E_INVALIDARG;
		/* Times count from the first sample's. */
		if (!d->based) {
			d->base = d->pending_timed ? d->pending_time : 0;
			d->based = TRUE;
		}
		while (d->pending_used < len && d->gathered < TSTA_FRAMES) {
			copy(d->gather + d->gathered * block,
			     src + d->pending_used, block);
			d->pending_used += block;
			d->gathered++;
		}
		if (d->gathered == TSTA_FRAMES) {
			if (FAILED(hr = emit_packet(d, ob)))
				return hr;
			if (d->pending_used < len)
				ob->dwStatus |= DMO_OUTPUT_DATA_BUFFERF_INCOMPLETE;
			else
				goto used_up;
			return S_OK;
		}
used_up:
		d->pending->lpVtbl->Release(d->pending);
		d->pending = NULL;
		if (ob->dwStatus)
			return S_OK;
	}
	if (d->draining && d->gathered)
		return emit_packet(d, ob);
	return S_FALSE;
}

/* PCM samples as floats in [-1, 1). */
static void to_float(const BYTE *pcm, DWORD samples, DWORD bits, float *out)
{
	for (DWORD i = 0; i < samples; i++, pcm += bits / 8) {
		LONG v;
		if (bits == 8)
			v = (LONG)pcm[0] - 128;
		else if (bits == 16)
			v = (SHORT)(pcm[0] | pcm[1] << 8);
		else if (bits == 24)
			v = (LONG)((DWORD)pcm[0] << 8 | (DWORD)pcm[1] << 16 |
				   (DWORD)pcm[2] << 24) >> 8;
		else
			v = (LONG)((DWORD)pcm[0] | (DWORD)pcm[1] << 8 |
				   (DWORD)pcm[2] << 16 | (DWORD)pcm[3] << 24);
		out[i] = (float)v / (float)(1u << (bits - 1));
	}
}

/* The decoder: the next PCM to give, decoding the input if need be. */
static HRESULT fill_ready(struct admo *d)
{
	WAVEFORMATEX *in = type_wfx(&d->in);

	if (d->pending) {
		BYTE *src = NULL;
		DWORD len = 0;
		HRESULT hr = d->pending->lpVtbl->GetBufferAndLength(d->pending,
								    &src, &len);
		if (FAILED(hr))
			return hr;
		LOWPTR(d, src);
		LONG frames = tsta_decode(src, len, in->wBitsPerSample,
					  in->nChannels, d->spare, TSTA_FRAMES);
		BOOL timed = d->pending_timed;
		REFERENCE_TIME time = d->pending_time;
		d->pending->lpVtbl->Release(d->pending);
		d->pending = NULL;
		if (frames < 0)
			return E_INVALIDARG;
		DWORD packet = d->packets++;
		BYTE *t = d->ready;
		if (d->hold) {
			/* The held packet goes out; this one is held. */
			BOOL had = d->have_held;
			d->ready = d->held;
			d->ready_frames = had ? d->held_frames : 0;
			d->ready_timed = d->held_timed;
			d->ready_time = d->held_time;
			d->ready_packet = d->held_packet;
			d->held = d->spare;
			d->spare = t;
			d->held_frames = (DWORD)frames;
			d->held_timed = timed;
			d->held_time = time;
			d->held_packet = packet;
			d->have_held = TRUE;
		} else {
			d->ready = d->spare;
			d->spare = t;
			d->ready_frames = (DWORD)frames;
			d->ready_timed = timed;
			d->ready_time = time;
			d->ready_packet = packet;
		}
		d->ready_pos = 0;
	} else if (d->draining && d->have_held) {
		BYTE *t = d->ready;
		d->ready = d->held;
		d->held = t;
		d->ready_frames = d->held_frames;
		d->ready_timed = d->held_timed;
		d->ready_time = d->held_time;
		d->ready_packet = d->held_packet;
		d->ready_pos = 0;
		d->have_held = FALSE;
	}
	return d->ready_pos < d->ready_frames ? S_OK : S_FALSE;
}

static HRESULT decode_output(struct admo *d, DMO_OUTPUT_DATA_BUFFER *ob)
{
	WAVEFORMATEX *in = type_wfx(&d->in);
	DWORD block = block_of(d);
	HRESULT hr;

	if (d->ready_pos >= d->ready_frames &&
	    (hr = fill_ready(d)) != S_OK)
		return hr;
	DWORD n = d->ready_frames - d->ready_pos;
	if (d->split && n > PIECE)
		n = PIECE;
	BOOL as_float = d->to_float && d->ready_packet >= 1;
	BYTE *dst;
	if (FAILED(hr = output_memory(d, ob, as_float ? n * 4 * in->nChannels :
				      n * block, &dst)))
		return hr;
	const BYTE *src = d->ready + d->ready_pos * block;
	if (as_float) {
		to_float(src, n * in->nChannels, in->wBitsPerSample,
			 (float *)dst);
		ob->pBuffer->lpVtbl->SetLength(ob->pBuffer,
					       n * 4 * in->nChannels);
	} else {
		copy(dst, src, n * block);
		ob->pBuffer->lpVtbl->SetLength(ob->pBuffer, n * block);
	}
	if ((d->to_float || d->rate) && d->ready_packet >= 1)
		d->switched = TRUE;
	if (d->ready_timed)
		stamp(ob, d->ready_time + (REFERENCE_TIME)tsta_ticks(
			      d->ready_pos, in->nSamplesPerSec),
		      d->ready_time + (REFERENCE_TIME)tsta_ticks(
			      d->ready_pos + n, in->nSamplesPerSec));
	else
		ob->dwStatus |= DMO_OUTPUT_DATA_BUFFERF_SYNCPOINT;
	d->ready_pos += n;
	if (d->ready_pos < d->ready_frames || (d->draining && d->have_held))
		ob->dwStatus |= DMO_OUTPUT_DATA_BUFFERF_INCOMPLETE;
	return S_OK;
}

static HRESULT WINAPI dmo_process_output(IMediaObject *p, DWORD flags,
					 DWORD count,
					 DMO_OUTPUT_DATA_BUFFER *buffers,
					 DWORD *status)
{
	struct admo *d = from(p);

	if (!buffers || !status || count != 1)
		return E_POINTER;
	LOWPTR(d, buffers);
	*status = 0;
	buffers[0].dwStatus = 0;
	if (!d->out_set)
		return DMO_E_TYPE_NOT_SET;
	if (d->encoder ? !d->gather : !d->ready)
		return S_FALSE;
	return d->encoder ? encode_output(d, &buffers[0]) :
	       decode_output(d, &buffers[0]);
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

/* ICodecAPI: the encoder's mean bit rate, which its format then gives. */

#define FROM_API(p) \
	((struct admo *)((BYTE *)(p) - FIELD_OFFSET(struct admo, api)))

static HRESULT WINAPI api_qi(ICodecAPI *p, REFIID i, void **o)
{
	return dmo_qi(&FROM_API(p)->iface, i, o);
}
static ULONG WINAPI api_addref(ICodecAPI *p)
{
	return dmo_addref(&FROM_API(p)->iface);
}
static ULONG WINAPI api_release(ICodecAPI *p)
{
	return dmo_release(&FROM_API(p)->iface);
}
static HRESULT WINAPI api_supported(ICodecAPI *p, const GUID *a)
{
	return a && eq(a, &api_mean_bitrate) ? S_OK : S_FALSE;
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
	if (!eq(a, &api_mean_bitrate))
		return E_NOTIMPL;
	v->vt = VT_UI4;
	v->ulVal = FROM_API(p)->bitrate;
	return S_OK;
}
static HRESULT WINAPI api_set(ICodecAPI *p, const GUID *a, VARIANT *v)
{
	if (!a || !v)
		return E_POINTER;
	if (!eq(a, &api_mean_bitrate) || v->vt != VT_UI4)
		return E_INVALIDARG;
	FROM_API(p)->bitrate = v->ulVal;
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
	FROM_API(p)->bitrate = 0;
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

HRESULT testdmo_audio_create(BOOL encoder, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	struct admo *d = alloc(sizeof(*d));
	if (!d)
		return E_OUTOFMEMORY;
	d->iface.lpVtbl = (IMediaObjectVtbl *)&dmo_vtbl;
	d->api.lpVtbl = &api_vtbl;
	d->refs = 1;
	d->encoder = encoder;
	d->split = env_on("TESTDMO_AUDIO_SPLIT");
	d->hold = env_on("TESTDMO_AUDIO_HOLD");
	d->to_float = env_on("TESTDMO_AUDIO_FLOAT");
	d->rate = env_on("TESTDMO_AUDIO_RATE");
	d->refuse = env_on("TESTDMO_NOTACCEPTING");
	d->strict = env_on("TESTDMO_STRICT");
	InterlockedIncrement(&objects);
	HRESULT hr = dmo_qi(&d->iface, iid, out);
	dmo_release(&d->iface);
	return hr;
}

LONG testdmo_audio_objects(void)
{
	return objects;
}
