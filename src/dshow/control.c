/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Optional encoder interfaces.  Keeping these operations here, rather than in
 * cmd_encode, means command-line policy never leaks into the COM host.
 */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "dshow/dshow.h"
#include "win32/k32.h"
#include "win32/ole.h"

static HRESULT persist_stream(IBaseFilter *filter, IPersistStream **out)
{
	if (!filter || !out)
		return E_POINTER;
	*out = NULL;
	return filter->lpVtbl->QueryInterface(filter, &IID_IPersistStream,
					      (void **)out);
}

HRESULT dshow_encoder_configure(IBaseFilter *filter,
				const struct dshow_encoder_options *options)
{
	IAMVideoCompression *compression = NULL;
	HRESULT first = S_OK;

	if (!filter || !options)
		return E_POINTER;
	HRESULT hr = filter->lpVtbl->QueryInterface(filter,
						    &IID_IAMVideoCompression,
						    (void **)&compression);
	if (FAILED(hr))
		return hr;

	if (options->key_interval >= 0) {
		LONG interval = options->key_interval;
		hr = compression->lpVtbl->put_KeyFrameRate(compression, interval);
		if (FAILED(hr) && SUCCEEDED(first))
			first = hr;
		/* Most filters expose one of these two equivalent controls. */
		hr = compression->lpVtbl->put_PFramesPerKeyFrame(compression,
							 interval ? interval - 1 : 0);
		if (FAILED(hr) && SUCCEEDED(first))
			first = hr;
	}
	if (options->quality >= 0.0) {
		double quality = options->quality;
		if (quality > 1.0)
			quality = 1.0;
		hr = compression->lpVtbl->put_Quality(compression, quality);
		if (FAILED(hr) && SUCCEEDED(first))
			first = hr;
	}
	if (options->bitrate >= 0) {
		/* WindowSize is the only standard 64-bit rate/size control here. */
		hr = compression->lpVtbl->put_WindowSize(compression,
							(DWORDLONG)options->bitrate);
		if (FAILED(hr) && SUCCEEDED(first))
			first = hr;
	}
	compression->lpVtbl->Release(compression);
	return first;
}

HRESULT dshow_filter_load_state(IBaseFilter *filter, const void *data,
				size_t size)
{
	IPersistStream *persist;
	IStream *stream = NULL;
	HGLOBAL block = NULL;
	HRESULT hr;

	if ((!data && size) || size > ULONG_MAX)
		return E_INVALIDARG;
	if (FAILED(hr = persist_stream(filter, &persist)))
		return hr;
	if (!(block = global_alloc(GMEM_MOVEABLE | GMEM_NODISCARD, size))) {
		hr = E_OUTOFMEMORY;
		goto done;
	}
	if (size) {
		void *p = global_lock(block);
		if (!p) {
			hr = E_OUTOFMEMORY;
			goto done;
		}
		memcpy(p, data, size);
		global_unlock(block);
	}
	hr = ole_stream_on_hglobal(block, FALSE, &stream);
	if (SUCCEEDED(hr))
		hr = persist->lpVtbl->Load(persist, stream);

done:
	if (stream)
		stream->lpVtbl->Release(stream);
	if (block)
		global_free(block);
	persist->lpVtbl->Release(persist);
	return hr;
}

HRESULT dshow_filter_save_state(IBaseFilter *filter, void **data, size_t *size)
{
	IPersistStream *persist;
	IStream *stream = NULL;
	HGLOBAL block = NULL;
	void *copy = NULL;
	HRESULT hr;

	if (!data || !size)
		return E_POINTER;
	*data = NULL;
	*size = 0;
	if (FAILED(hr = persist_stream(filter, &persist)))
		return hr;
	if (FAILED(hr = ole_stream_on_hglobal(NULL, FALSE, &stream)))
		goto done;
	if (FAILED(hr = persist->lpVtbl->Save(persist, stream, TRUE)))
		goto done;
	if (FAILED(hr = ole_hglobal_from_stream(stream, &block)))
		goto done;

	size_t n = global_size(block);
	if (n && !(copy = malloc(n))) {
		hr = E_OUTOFMEMORY;
		goto done;
	}
	if (n) {
		void *p = global_lock(block);
		if (!p) {
			free(copy);
			copy = NULL;
			hr = E_OUTOFMEMORY;
			goto done;
		}
		memcpy(copy, p, n);
		global_unlock(block);
	}
	*data = copy;
	*size = n;
	hr = S_OK;

done:
	if (stream)
		stream->lpVtbl->Release(stream);
	if (block)
		global_free(block);
	persist->lpVtbl->Release(persist);
	return hr;
}
