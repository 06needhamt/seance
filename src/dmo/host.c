/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The DMO host: sets a DMO's input and output types, then feeds it one
 * buffer at a time with ProcessInput and takes every output it has with
 * ProcessOutput, handing each to a callback. A DMO that isn't accepting
 * input (DMO_E_NOTACCEPTING) is drained first; one that has more output
 * (DMO_OUTPUT_DATA_BUFFERF_INCOMPLETE) is asked again; a DMO that changes
 * its output type has the new type passed on with the next output.
 *
 * DMOs know no preroll, and needn't stamp their outputs, so the host keeps
 * what it pushed and gives each output the preroll flag (and, if the DMO
 * didn't stamp it, the times) of the input it came from: the one pushed
 * with its time stamp, or for an unstamped output the oldest.
 *
 * Every call into the DMO is crash-framed and traced under VFW_TRACE, so a
 * crash report names the method ("IMediaObject::ProcessOutput").
 */
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/log.h"
#include "dmo/dmo.h"
#include "dshow/dshow.h"
#include "loader/modules.h"
#include "runtime/crash.h"
#include "runtime/runtime.h"
#include "win32/ole.h"

/* A DMO saying "incomplete" this often in a row with nothing to show. */
#define MAX_EMPTY 64
/* Inputs remembered while waiting for what comes of them. */
#define MAX_PENDING 256

#define CALL(method, fn, expression) ({ \
	struct crash_frame saved_; \
	char name_[24]; \
	TRACE("dmo: %s", method); \
	crash_enter(method, (const void *)(fn), 0, NULL, &saved_); \
	HRESULT hr_ = (expression); \
	crash_leave(&saved_); \
	TRACE("dmo: %s = %s", method, dshow_hr_name(hr_, name_)); \
	hr_; \
})

#define MO(h, method, ...) \
	CALL("IMediaObject::" #method, (h)->lpVtbl->method, \
	     (h)->lpVtbl->method((h), ##__VA_ARGS__))

struct pending {
	REFERENCE_TIME start, stop;
	int preroll;
};

/*
 * Everything handed to the DMO lives in here, in the harness's heap (below
 * 2 GB), never on the caller's stack: a DMO may keep or check a pointer as
 * a signed int.
 */
struct dmo_host {
	IMediaObject *mo;
	dmo_output_fn callback;
	void *opaque;
	DMO_MEDIA_TYPE input;
	DMO_MEDIA_TYPE output;		/* as set, codec data appended */
	DMO_MEDIA_TYPE current;		/* as last seen */
	DMO_MEDIA_TYPE scratch;		/* GetOutputCurrentType's answer */
	DMO_OUTPUT_DATA_BUFFER ob;
	DWORD status, nin, nout, size, align;
	IMediaBuffer *out;
	int streaming;
	struct pending *queue;
	size_t head, count, cap;
};

static int same_type(const DMO_MEDIA_TYPE *a, const DMO_MEDIA_TYPE *b)
{
	return !memcmp(&a->majortype, &b->majortype, sizeof(GUID)) &&
	       !memcmp(&a->subtype, &b->subtype, sizeof(GUID)) &&
	       !memcmp(&a->formattype, &b->formattype, sizeof(GUID)) &&
	       a->cbFormat == b->cbFormat &&
	       (!a->cbFormat || !memcmp(a->pbFormat, b->pbFormat, a->cbFormat));
}

/* What the output buffer must hold: the DMO's word, or the picture's size. */
static DWORD output_size(const DMO_MEDIA_TYPE *mt, DWORD asked)
{
	DWORD size = asked;

	if (mt->lSampleSize > size)
		size = mt->lSampleSize;
	BITMAPINFOHEADER *bih = NULL;
	size_t n = 0;
	if (SUCCEEDED(dshow_mt_to_bih(mt, &bih, &n))) {
		LONG h = bih->biHeight < 0 ? -bih->biHeight : bih->biHeight;
		DWORD guess = (DWORD)((((uint64_t)bih->biWidth * 32 + 31) / 32) *
				      4 * (uint64_t)h);
		if (bih->biSizeImage > size)
			size = bih->biSizeImage;
		if (!size)
			size = guess;
		free(bih);
	}
	return size ? size : 1 << 20;
}

/*
 * IWMCodecPrivateData: set the partial output type, then read the codec
 * data and give back the output type with it appended to the format (and,
 * for video, counted in biSize). S_FALSE with no change if the DMO has none.
 */
static HRESULT private_data(struct dmo_host *h, const DMO_MEDIA_TYPE *out)
{
	IMediaObject *mo = h->mo;
	DMO_MEDIA_TYPE *full = &h->output;
	IWMCodecPrivateData *pd = NULL;
	BYTE *data = NULL;
	DWORD *size = &h->size;

	HRESULT hr = dshow_copy_media_type(full, out);
	if (FAILED(hr))
		return hr;
	if (FAILED(mo->lpVtbl->QueryInterface(mo, &IID_IWMCodecPrivateData,
					      (void **)&pd)) || !pd)
		return S_FALSE;
	hr = CALL("IWMCodecPrivateData::SetPartialOutputType",
		  pd->lpVtbl->SetPartialOutputType,
		  pd->lpVtbl->SetPartialOutputType(pd, full));
	if (SUCCEEDED(hr))
		hr = CALL("IWMCodecPrivateData::GetPrivateData",
			  pd->lpVtbl->GetPrivateData,
			  pd->lpVtbl->GetPrivateData(pd, NULL, size));
	if (SUCCEEDED(hr) && *size) {
		if (!(data = malloc(*size)))
			hr = E_OUTOFMEMORY;
		else
			hr = CALL("IWMCodecPrivateData::GetPrivateData",
				  pd->lpVtbl->GetPrivateData,
				  pd->lpVtbl->GetPrivateData(pd, data, size));
	}
	pd->lpVtbl->Release(pd);
	if (FAILED(hr) || !*size) {
		free(data);
		return FAILED(hr) ? hr : S_FALSE;
	}

	DMO_MEDIA_TYPE grown = *full;
	grown.pbFormat = NULL;
	grown.pUnk = NULL;
	grown.cbFormat = full->cbFormat + *size;
	if (!(grown.pbFormat = ole_taskmem_alloc(grown.cbFormat))) {
		free(data);
		return E_OUTOFMEMORY;
	}
	memcpy(grown.pbFormat, full->pbFormat, full->cbFormat);
	memcpy(grown.pbFormat + full->cbFormat, data, *size);
	free(data);
	BITMAPINFOHEADER *bih = NULL;
	if (!memcmp(&grown.formattype, &FORMAT_VideoInfo, sizeof(GUID)) &&
	    grown.cbFormat >= sizeof(VIDEOINFOHEADER))
		bih = &((VIDEOINFOHEADER *)grown.pbFormat)->bmiHeader;
	else if (!memcmp(&grown.formattype, &FORMAT_VideoInfo2, sizeof(GUID)) &&
		 grown.cbFormat >= sizeof(VIDEOINFOHEADER2))
		bih = &((VIDEOINFOHEADER2 *)grown.pbFormat)->bmiHeader;
	if (bih)
		bih->biSize += *size;
	dshow_free_media_type(full);
	*full = grown;
	return S_OK;
}

HRESULT dmo_host_create(IMediaObject *mo, const DMO_MEDIA_TYPE *input,
			const DMO_MEDIA_TYPE *output, dmo_output_fn callback,
			void *opaque, struct dmo_host **out)
{
	HRESULT hr;

	if (!mo || !input || !output || !callback || !out)
		return E_POINTER;
	*out = NULL;
	if (runtime_enter())
		return E_FAIL;
	struct dmo_host *h = calloc(1, sizeof(*h));
	if (!h)
		return E_OUTOFMEMORY;
	h->mo = mo;
	h->callback = callback;
	h->opaque = opaque;
	mo->lpVtbl->AddRef(mo);

	if (FAILED(hr = MO(mo, GetStreamCount, &h->nin, &h->nout)))
		goto fail;
	if (!h->nin || !h->nout) {
		hr = DMO_E_INVALIDSTREAMINDEX;
		goto fail;
	}
	if (FAILED(hr = dshow_copy_media_type(&h->input, input)) ||
	    FAILED(hr = MO(mo, SetInputType, 0, &h->input,
			   DMO_SET_TYPEF_TEST_ONLY)) ||
	    FAILED(hr = MO(mo, SetInputType, 0, &h->input, 0)) ||
	    FAILED(hr = private_data(h, output)) ||
	    FAILED(hr = MO(mo, SetOutputType, 0, &h->output,
			   DMO_SET_TYPEF_TEST_ONLY)) ||
	    FAILED(hr = MO(mo, SetOutputType, 0, &h->output, 0)) ||
	    FAILED(hr = MO(mo, GetOutputSizeInfo, 0, &h->size, &h->align)))
		goto fail;
	/* Changes are against what the DMO says it has, not what was set. */
	if (FAILED(MO(mo, GetOutputCurrentType, 0, &h->current)) &&
	    FAILED(hr = dshow_copy_media_type(&h->current, &h->output)))
		goto fail;
	hr = MO(mo, AllocateStreamingResources);
	if (FAILED(hr) && hr != E_NOTIMPL)
		goto fail;
	h->streaming = 1;
	if (FAILED(hr = dmo_buffer_create(output_size(&h->output, h->size),
					  h->align, &h->out)))
		goto fail;
	*out = h;
	return S_OK;

fail:
	dmo_host_destroy(h);
	return hr;
}

const DMO_MEDIA_TYPE *dmo_host_output_type(const struct dmo_host *host)
{
	return host ? &host->output : NULL;
}

/* Removes entry i (counted from the oldest) into *p. */
static void queue_take(struct dmo_host *h, size_t i, struct pending *p)
{
	*p = h->queue[(h->head + i) % h->cap];
	for (; i > 0; i--)
		h->queue[(h->head + i) % h->cap] =
			h->queue[(h->head + i - 1) % h->cap];
	h->head = (h->head + 1) % h->cap;
	h->count--;
}

/* What was pushed, in order. */
static int queue_push(struct dmo_host *h, REFERENCE_TIME start,
		      REFERENCE_TIME stop, int preroll)
{
	if (h->count == MAX_PENDING) {
		struct pending dropped;
		queue_take(h, 0, &dropped);
	}
	if (h->count == h->cap) {
		size_t cap = h->cap ? h->cap * 2 : 16;
		struct pending *q = malloc(cap * sizeof(*q));
		if (!q)
			return -1;
		for (size_t i = 0; i < h->count; i++)
			q[i] = h->queue[(h->head + i) % h->cap];
		free(h->queue);
		h->queue = q;
		h->head = 0;
		h->cap = cap;
	}
	h->queue[(h->head + h->count++) % h->cap] =
		(struct pending){ start, stop, preroll };
	return 0;
}

/*
 * The input an output came from: for a stamped output, the one pushed with
 * that time (an input that gave nothing, or a reordered one, leaves the
 * rest in step); else the oldest. An input that never gives anything is
 * forgotten once MAX_PENDING newer ones are waiting.
 */
static int queue_match(struct dmo_host *h, int stamped, REFERENCE_TIME time,
		       struct pending *p)
{
	if (!h->count)
		return 0;
	if (stamped) {
		for (size_t i = 0; i < h->count; i++)
			if (h->queue[(h->head + i) % h->cap].start == time) {
				queue_take(h, i, p);
				return 1;
			}
		return 0;
	}
	queue_take(h, 0, p);
	return 1;
}

/* Hands one output on, with the new output type if it has changed. */
static HRESULT deliver(struct dmo_host *h, const DMO_OUTPUT_DATA_BUFFER *ob)
{
	BYTE *data = NULL;
	DWORD length = 0;
	struct pending p = { 0, 0, 0 };
	struct dmo_output o;
	const DMO_MEDIA_TYPE *changed = NULL;

	HRESULT hr = h->out->lpVtbl->GetBufferAndLength(h->out, &data, &length);
	if (FAILED(hr))
		return hr;
	if (!length)
		return S_OK;
	memset(&h->scratch, 0, sizeof(h->scratch));
	if (SUCCEEDED(MO(h->mo, GetOutputCurrentType, 0, &h->scratch)) &&
	    !same_type(&h->scratch, &h->current)) {
		dshow_free_media_type(&h->current);
		h->current = h->scratch;
		memset(&h->scratch, 0, sizeof(h->scratch));
		changed = &h->current;
	}
	dshow_free_media_type(&h->scratch);

	int have = queue_match(h, !!(ob->dwStatus & DMO_OUTPUT_DATA_BUFFERF_TIME),
			       ob->rtTimestamp, &p);
	o.data = data;
	o.size = length;
	o.flags = ob->dwStatus;
	o.time = ob->rtTimestamp;
	o.length = ob->rtTimelength;
	o.preroll = have && p.preroll;
	if (!(o.flags & DMO_OUTPUT_DATA_BUFFERF_TIME) && have) {
		o.time = p.start;
		o.length = p.stop - p.start;
		o.flags |= DMO_OUTPUT_DATA_BUFFERF_TIME |
			   DMO_OUTPUT_DATA_BUFFERF_TIMELENGTH;
	}
	return h->callback(h->opaque, &o, changed);
}

/* Takes every output the DMO has. */
static HRESULT drain(struct dmo_host *h)
{
	int empty = 0;

	for (;;) {
		h->ob = (DMO_OUTPUT_DATA_BUFFER){ h->out, 0, 0, 0 };
		h->status = 0;
		h->out->lpVtbl->SetLength(h->out, 0);
		HRESULT hr = MO(h->mo, ProcessOutput, 0, 1, &h->ob, &h->status);
		if (FAILED(hr))
			return hr;
		int incomplete =
			!!(h->ob.dwStatus & DMO_OUTPUT_DATA_BUFFERF_INCOMPLETE);
		DWORD length = 0;
		h->out->lpVtbl->GetBufferAndLength(h->out, NULL, &length);
		if (hr == S_OK && length) {
			empty = 0;
			if (FAILED(hr = deliver(h, &h->ob)))
				return hr;
		} else if (++empty > MAX_EMPTY) {
			vfw_warn("dmo: the DMO keeps saying it has more output "
				 "and gives none");
			return E_FAIL;
		}
		if (!incomplete)
			return S_OK;
	}
}

HRESULT dmo_host_push(struct dmo_host *h, const void *data, size_t size,
		      REFERENCE_TIME start, REFERENCE_TIME stop, DWORD flags)
{
	IMediaBuffer *in = NULL;

	if (!h || (!data && size))
		return E_POINTER;
	if (size > 0xffffffffu)
		return E_INVALIDARG;
	HRESULT hr = dmo_buffer_from(data, (DWORD)size, &in);
	if (FAILED(hr))
		return hr;
	if (queue_push(h, start, stop, !!(flags & AM_SAMPLE_PREROLL))) {
		in->lpVtbl->Release(in);
		return E_OUTOFMEMORY;
	}
	DWORD in_flags = DMO_INPUT_DATA_BUFFERF_TIME |
			 DMO_INPUT_DATA_BUFFERF_TIMELENGTH;
	if (flags & AM_SAMPLE_SPLICEPOINT)
		in_flags |= DMO_INPUT_DATA_BUFFERF_SYNCPOINT;

	hr = MO(h->mo, ProcessInput, 0, in, in_flags, start, stop - start);
	if (hr == DMO_E_NOTACCEPTING) {
		/* It has output to give first; then it takes this. */
		if (SUCCEEDED(hr = drain(h)))
			hr = MO(h->mo, ProcessInput, 0, in, in_flags, start,
				stop - start);
	}
	in->lpVtbl->Release(in);
	if (FAILED(hr)) {
		/* Nothing will come of it. */
		if (h->count) {
			h->count--;
		}
		return hr;
	}
	return drain(h);
}

HRESULT dmo_host_flush(struct dmo_host *h)
{
	if (!h)
		return E_POINTER;
	h->head = h->count = 0;
	return MO(h->mo, Flush);
}

HRESULT dmo_host_end(struct dmo_host *h)
{
	if (!h)
		return E_POINTER;
	HRESULT hr = MO(h->mo, Discontinuity, 0);
	if (FAILED(hr))
		return hr;
	return drain(h);
}

void dmo_host_destroy(struct dmo_host *h)
{
	if (!h)
		return;
	if (h->streaming)
		MO(h->mo, FreeStreamingResources);
	if (h->out)
		h->out->lpVtbl->Release(h->out);
	h->mo->lpVtbl->Release(h->mo);
	dshow_free_media_type(&h->input);
	dshow_free_media_type(&h->output);
	dshow_free_media_type(&h->current);
	free(h->queue);
	free(h);
}

/* Opening a DMO from its DLL. */

struct dmo_object {
	struct module *module;
};

HRESULT dmo_object_open(const char *path, const CLSID *clsid,
			struct dmo_object **object, IMediaObject **mo)
{
	IClassFactory *factory = NULL;
	HRESULT hr = CLASS_E_CLASSNOTAVAILABLE;

	if (!path || !clsid || !object || !mo)
		return E_POINTER;
	*object = NULL;
	*mo = NULL;
	if (runtime_enter())
		return E_FAIL;
	struct dmo_object *o = calloc(1, sizeof(*o));
	if (!o)
		return E_OUTOFMEMORY;
	if (!(o->module = mod_load(path, NULL))) {
		free(o);
		return E_FAIL;
	}
	LPFNGETCLASSOBJECT get = (LPFNGETCLASSOBJECT)mod_get_proc(
		o->module, "DllGetClassObject");
	if (get)
		hr = CALL("DllGetClassObject", get,
			  get(clsid, &IID_IClassFactory, (void **)&factory));
	if (SUCCEEDED(hr))
		hr = CALL("IClassFactory::CreateInstance",
			  factory->lpVtbl->CreateInstance,
			  factory->lpVtbl->CreateInstance(factory, NULL,
							  &IID_IMediaObject,
							  (void **)mo));
	if (factory)
		factory->lpVtbl->Release(factory);
	if (FAILED(hr)) {
		*mo = NULL;
		mod_free(o->module);
		free(o);
		return hr;
	}
	*object = o;
	return S_OK;
}

void dmo_object_close(struct dmo_object *object, IMediaObject *mo)
{
	if (mo)
		mo->lpVtbl->Release(mo);
	if (object) {
		mod_free(object->module);
		free(object);
	}
}
