/* SPDX-License-Identifier: GPL-2.0-only */
/* Filters and DMOs behind one interface for the commands (see xform.h). */
#include <stdlib.h>
#include <string.h>

#include "dmo/discover.h"
#include "dmo/dmo.h"
#include "dshow/discover.h"
#include "dshow/dshow.h"
#include "xform.h"

struct xform {
	struct xform_class c;
	/* a filter */
	struct dshow_filter *server;
	IBaseFilter *filter;
	struct dshow_host *host;
	AM_MEDIA_TYPE out;
	/* a DMO */
	struct dmo_object *object;
	IMediaObject *mo;
	struct dmo_host *dmo_host;
	/* the caller's */
	xform_output_fn callback;
	void *opaque;
};

/*
 * An encoder's output: a FOURCC-shaped subtype for video, a format tag's
 * subtype for audio; 0 if g is neither.
 */
static DWORD compression_of(const GUID *g, int audio)
{
	static const BYTE tail[8] = { 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71 };

	if (g->Data2 || g->Data3 != 0x10 || memcmp(g->Data4, tail, sizeof(tail)))
		return 0;
	if (audio)
		return g->Data1 && g->Data1 <= 0xffff &&
		       g->Data1 != WAVE_FORMAT_PCM &&
		       g->Data1 != WAVE_FORMAT_IEEE_FLOAT ? g->Data1 : 0;
	return g->Data1 > 0xffff ? g->Data1 : 0;
}

static DWORD filter_compression(const struct qz_filter *f, int audio)
{
	for (size_t p = 0; p < f->npins; p++) {
		if (!(f->pins[p].flags & REG_PINFLAG_B_OUTPUT))
			continue;
		for (size_t t = 0; t < f->pins[p].ntypes; t++) {
			DWORD c = compression_of(&f->pins[p].types[t].minor,
						 audio);
			if (c)
				return c;
		}
	}
	return 0;
}

static DWORD dmo_compression(const struct dmo_reg *d, int audio)
{
	for (size_t t = 0; t < d->nout; t++) {
		DWORD c = compression_of(&d->out[t].subtype, audio);
		if (c)
			return c;
	}
	return 0;
}

static enum dmo_role dmo_codec_role(int audio, int encoder)
{
	return audio ? (encoder ? DMO_ROLE_AUDIO_ENCODER : DMO_ROLE_AUDIO_DECODER) :
		       (encoder ? DMO_ROLE_VIDEO_ENCODER : DMO_ROLE_VIDEO_DECODER);
}

HRESULT xform_find(const char *path, const CLSID *selected, int audio,
		   int encoder, struct xform_class *found)
{
	struct dshow_server_found f;
	HRESULT registration;
	int matches = 0;

	if (!path || !found)
		return E_POINTER;
	memset(found, 0, sizeof(*found));
	if (dshow_server_discover(path, &f, &registration))
		return E_FAIL;
	for (size_t i = 0; i < f.nfilters; i++) {
		const struct qz_filter *q = &f.filters[i];
		if ((selected && memcmp(selected, &q->clsid, sizeof(*selected))) ||
		    dshow_role(q) != dshow_codec_role(audio, encoder))
			continue;
		DWORD c = encoder ? filter_compression(q, audio) : 0;
		if (encoder && !c)
			continue;
		found->clsid = q->clsid;
		found->dmo = 0;
		found->compression = c;
		matches++;
	}
	for (size_t i = 0; i < f.ndmos; i++) {
		const struct dmo_reg *d = &f.dmos[i];
		if ((selected && memcmp(selected, &d->clsid, sizeof(*selected))) ||
		    dmo_role(d) != dmo_codec_role(audio, encoder))
			continue;
		/* One DMO in several categories is still one. */
		if (matches && found->dmo &&
		    !memcmp(&found->clsid, &d->clsid, sizeof(CLSID)))
			continue;
		DWORD c = encoder ? dmo_compression(d, audio) : 0;
		if (encoder && !c)
			continue;
		found->clsid = d->clsid;
		found->dmo = 1;
		found->compression = c;
		matches++;
	}
	dshow_server_found_free(&f);
	if (matches == 1)
		return S_OK;
	return matches ? VFW_E_DUPLICATE_NAME : CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT xform_create(const char *path, const struct xform_class *c,
		     struct xform **out)
{
	HRESULT hr;

	if (!path || !c || !out)
		return E_POINTER;
	*out = NULL;
	struct xform *x = calloc(1, sizeof(*x));
	if (!x)
		return E_OUTOFMEMORY;
	x->c = *c;
	hr = c->dmo ? dmo_object_open(path, &c->clsid, &x->object, &x->mo) :
		      dshow_filter_create(path, &c->clsid, &x->server,
					  &x->filter);
	if (FAILED(hr)) {
		free(x);
		return hr;
	}
	*out = x;
	return S_OK;
}

HRESULT xform_open(const char *path, const CLSID *selected, int audio,
		   int encoder, struct xform **x, struct xform_class *found)
{
	struct xform_class c;
	HRESULT hr = xform_find(path, selected, audio, encoder, &c);

	if (x)
		*x = NULL;
	if (FAILED(hr))
		return hr;
	if (found)
		*found = c;
	return xform_create(path, &c, x);
}

void xform_close(struct xform *x)
{
	if (!x)
		return;
	xform_disconnect(x);
	if (x->c.dmo)
		dmo_object_close(x->object, x->mo);
	else
		dshow_filter_close(x->server, x->filter);
	free(x);
}

const char *xform_kind_name(const struct xform *x)
{
	return x && x->c.dmo ? "DMO" : "DirectShow filter";
}

void *xform_object(const struct xform *x)
{
	if (!x)
		return NULL;
	return x->c.dmo ? (void *)x->mo : (void *)x->filter;
}

/* The two hosts' outputs, as one. */

static HRESULT from_sample(void *opaque, IMediaSample *sample,
			   const AM_MEDIA_TYPE *new_type)
{
	struct xform *x = opaque;
	struct xform_output o = { 0 };
	BYTE *data = NULL;
	LONG size = sample->lpVtbl->GetActualDataLength(sample);

	if (size < 0 || FAILED(sample->lpVtbl->GetPointer(sample, &data)))
		return VFW_E_INVALIDMEDIATYPE;
	o.data = data;
	o.size = (size_t)size;
	o.sync = sample->lpVtbl->IsSyncPoint(sample) == S_OK;
	o.preroll = sample->lpVtbl->IsPreroll(sample) == S_OK;
	o.timed = SUCCEEDED(sample->lpVtbl->GetTime(sample, &o.start, &o.stop));
	return x->callback ? x->callback(x->opaque, &o, new_type) : S_OK;
}

static HRESULT from_dmo(void *opaque, const struct dmo_output *d,
			const DMO_MEDIA_TYPE *new_type)
{
	struct xform *x = opaque;
	struct xform_output o = { 0 };

	o.data = d->data;
	o.size = d->size;
	o.sync = !!(d->flags & DMO_OUTPUT_DATA_BUFFERF_SYNCPOINT);
	o.preroll = d->preroll;
	o.timed = !!(d->flags & DMO_OUTPUT_DATA_BUFFERF_TIME);
	o.start = d->time;
	o.stop = d->flags & DMO_OUTPUT_DATA_BUFFERF_TIMELENGTH ?
		 d->time + d->length : d->time;
	return x->callback ? x->callback(x->opaque, &o, new_type) : S_OK;
}

HRESULT xform_connect(struct xform *x, const AM_MEDIA_TYPE *in,
		      const AM_MEDIA_TYPE *out, xform_output_fn callback,
		      void *opaque)
{
	HRESULT hr;

	if (!x || !in || !out)
		return E_POINTER;
	xform_disconnect(x);
	x->callback = callback;
	x->opaque = opaque;
	if (x->c.dmo)
		return dmo_host_create(x->mo, in, out, from_dmo, x, &x->dmo_host);
	hr = dshow_host_create(x->filter, in, out, callback ? from_sample : NULL,
			       x, &x->host);
	/* The type connected: a partial one, completed by the filter. */
	if (SUCCEEDED(hr))
		hr = dshow_copy_media_type(&x->out,
					   dshow_host_output_type(x->host));
	if (FAILED(hr)) {
		dshow_host_destroy(x->host);
		x->host = NULL;
		dshow_free_media_type(&x->out);
	}
	return hr;
}

const AM_MEDIA_TYPE *xform_output_type(const struct xform *x)
{
	if (!x)
		return NULL;
	if (x->c.dmo)
		return dmo_host_output_type(x->dmo_host);
	return x->host ? &x->out : NULL;
}

void xform_disconnect(struct xform *x)
{
	if (!x)
		return;
	dshow_host_destroy(x->host);
	x->host = NULL;
	dmo_host_destroy(x->dmo_host);
	x->dmo_host = NULL;
	dshow_free_media_type(&x->out);
}

HRESULT xform_push(struct xform *x, const void *data, size_t size,
		   REFERENCE_TIME start, REFERENCE_TIME stop, DWORD flags)
{
	if (!x)
		return E_POINTER;
	if (x->dmo_host)
		return dmo_host_push(x->dmo_host, data, size, start, stop, flags);
	if (x->host)
		return dshow_host_push(x->host, data, size, start, stop, flags);
	return VFW_E_NOT_CONNECTED;
}

HRESULT xform_seek(struct xform *x, REFERENCE_TIME start, REFERENCE_TIME stop)
{
	if (!x)
		return E_POINTER;
	if (x->dmo_host)
		return dmo_host_flush(x->dmo_host);
	if (x->host)
		return dshow_host_seek(x->host, start, stop);
	return VFW_E_NOT_CONNECTED;
}

HRESULT xform_end(struct xform *x)
{
	if (!x)
		return E_POINTER;
	if (x->dmo_host)
		return dmo_host_end(x->dmo_host);
	if (x->host)
		return dshow_host_end(x->host, 0);
	return VFW_E_NOT_CONNECTED;
}
