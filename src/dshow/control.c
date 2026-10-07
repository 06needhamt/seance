/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Optional encoder interfaces.  Keeping these operations here, rather than in
 * cmd_encode, means command-line policy never leaks into the COM host. Each
 * takes any COM object, a DirectShow filter or a DMO, and asks it for the
 * interface it needs.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dshow/dshow.h"
#include "win32/k32.h"
#include "win32/ole.h"

static HRESULT persist_stream(void *object, IPersistStream **out)
{
	IUnknown *unknown = object;

	if (!unknown || !out)
		return E_POINTER;
	*out = NULL;
	return unknown->lpVtbl->QueryInterface(unknown, &IID_IPersistStream,
					       (void **)out);
}

int dshow_filter_interfaces(void *object, char *buf, size_t size)
{
	static const struct {
		const IID *iid;
		const char *name;
	} known[] = {
		{ &IID_IAMVideoCompression, "IAMVideoCompression" },
		{ &IID_ICodecAPI, "ICodecAPI" },
		{ &IID_IPersistStream, "IPersistStream" },
		{ &IID_ISpecifyPropertyPages, "ISpecifyPropertyPages" },
		{ &IID_IWMCodecPrivateData, "IWMCodecPrivateData" },
		{ &IID_IPropertyBag, "IPropertyBag" },
	};
	IUnknown *filter = object;
	int found = 0;
	size_t used = 0;

	if (size)
		buf[0] = '\0';
	if (!filter)
		return 0;
	for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
		IUnknown *unknown = NULL;

		if (FAILED(filter->lpVtbl->QueryInterface(filter, known[i].iid,
							  (void **)&unknown)) || !unknown)
			continue;
		unknown->lpVtbl->Release(unknown);
		if (size > used) {
			int n = snprintf(buf + used, size - used, "%s%s",
					 found ? ", " : "", known[i].name);
			if (n > 0)
				used += (size_t)n < size - used ? (size_t)n : size - used - 1;
		}
		found++;
	}
	return found;
}

/*
 * One ICodecAPI setting, an unsigned 32-bit value. S_FALSE if the filter
 * doesn't have the setting (IsSupported says so) or won't let it be changed.
 */
static HRESULT codec_api_set(ICodecAPI *api, const GUID *setting, ULONG value)
{
	VARIANT v;
	HRESULT hr = api->lpVtbl->IsSupported(api, setting);

	if (hr != S_OK)
		return FAILED(hr) ? hr : S_FALSE;
	if (api->lpVtbl->IsModifiable(api, setting) != S_OK)
		return S_FALSE;
	memset(&v, 0, sizeof(v));
	v.vt = VT_UI4;
	v.ulVal = value;
	return api->lpVtbl->SetValue(api, setting, &v);
}

/*
 * Makes the mean bit rate count. It applies only in the constant and variable
 * bit rate modes, so an encoder in quality mode (which is what some default
 * to) would take the setting and ignore it. Such an encoder is switched to
 * constant bit rate; one in another mode, or with no rate-control mode at
 * all, is left as it is. S_OK, or a failure if it needs the switch and can't.
 */
static HRESULT codec_api_rate_mode(ICodecAPI *api)
{
	VARIANT v;
	HRESULT hr = api->lpVtbl->IsSupported(api, &CODECAPI_AVEncCommonRateControlMode);

	if (hr != S_OK)
		return FAILED(hr) ? hr : S_OK;
	memset(&v, 0, sizeof(v));
	hr = api->lpVtbl->GetValue(api, &CODECAPI_AVEncCommonRateControlMode, &v);
	if (SUCCEEDED(hr) && (v.vt == VT_UI4 || v.vt == VT_I4) &&
	    v.ulVal != eAVEncCommonRateControlMode_Quality)
		return S_OK;
	/* In quality mode, or can't tell: switch. */
	hr = codec_api_set(api, &CODECAPI_AVEncCommonRateControlMode,
			   eAVEncCommonRateControlMode_CBR);
	return hr == S_FALSE ? E_NOTIMPL : hr;
}

/*
 * Applies the options through the interfaces the filter has: IAMVideoCompression
 * (the DirectShow one) and ICodecAPI (the Media Foundation one carried over).
 * An option that either takes is applied; one that none takes is an error, as
 * is a filter with neither.
 */
HRESULT dshow_encoder_configure(void *object,
				const struct dshow_encoder_options *options)
{
	IAMVideoCompression *compression = NULL;
	ICodecAPI *api = NULL;
	HRESULT first = S_OK, hr;
	int have_key = 0, have_quality = 0, have_rate = 0;
	IUnknown *filter = object;

	if (!filter || !options)
		return E_POINTER;
	hr = filter->lpVtbl->QueryInterface(filter, &IID_IAMVideoCompression,
					    (void **)&compression);
	if (FAILED(hr))
		compression = NULL;
	hr = filter->lpVtbl->QueryInterface(filter, &IID_ICodecAPI, (void **)&api);
	if (FAILED(hr))
		api = NULL;
	if (!compression && !api)
		return E_NOINTERFACE;

	/* The first failure of an option, kept if nothing else takes it. */
	HRESULT key_error = S_OK, quality_error = S_OK, rate_error = S_OK;

	if (options->key_interval >= 0) {
		LONG interval = options->key_interval;

		if (compression) {
			/* Most filters expose one of these two equivalent controls. */
			hr = compression->lpVtbl->put_KeyFrameRate(compression, interval);
			if (SUCCEEDED(hr))
				have_key = 1;
			else
				key_error = hr;
			hr = compression->lpVtbl->put_PFramesPerKeyFrame(compression,
						interval ? interval - 1 : 0);
			if (SUCCEEDED(hr))
				have_key = 1;
			else if (SUCCEEDED(key_error))
				key_error = hr;
		}
		if (api) {
			hr = codec_api_set(api, &CODECAPI_AVEncMPVGOPSize, interval);
			if (hr == S_OK)
				have_key = 1;
			else if (FAILED(hr) && SUCCEEDED(key_error))
				key_error = hr;
		}
	}
	if (options->quality >= 0.0) {
		double quality = options->quality > 1.0 ? 1.0 : options->quality;

		if (compression) {
			hr = compression->lpVtbl->put_Quality(compression, quality);
			if (SUCCEEDED(hr))
				have_quality = 1;
			else
				quality_error = hr;
		}
		if (api) {
			/* ICodecAPI's quality is a percentage. */
			hr = codec_api_set(api, &CODECAPI_AVEncCommonQuality,
					   (ULONG)(quality * 100.0 + 0.5));
			if (hr == S_OK)
				have_quality = 1;
			else if (FAILED(hr) && SUCCEEDED(quality_error))
				quality_error = hr;
		}
	}
	if (options->bitrate >= 0) {
		if (compression) {
			/* WindowSize is the only standard 64-bit rate/size control here. */
			hr = compression->lpVtbl->put_WindowSize(compression,
						(DWORDLONG)options->bitrate);
			if (SUCCEEDED(hr))
				have_rate = 1;
			else
				rate_error = hr;
		}
		if (api) {
			hr = codec_api_rate_mode(api);
			if (SUCCEEDED(hr))
				hr = codec_api_set(api, &CODECAPI_AVEncCommonMeanBitRate,
						   options->bitrate > ULONG_MAX ? ULONG_MAX :
						   (ULONG)options->bitrate);
			if (hr == S_OK)
				have_rate = 1;
			else if (FAILED(hr) && SUCCEEDED(rate_error))
				rate_error = hr;
		}
	}

	/* An option nobody took fails with the first reason given, else E_NOTIMPL. */
	if (options->key_interval >= 0 && !have_key)
		first = FAILED(key_error) ? key_error : E_NOTIMPL;
	if (SUCCEEDED(first) && options->quality >= 0.0 && !have_quality)
		first = FAILED(quality_error) ? quality_error : E_NOTIMPL;
	if (SUCCEEDED(first) && options->bitrate >= 0 && !have_rate)
		first = FAILED(rate_error) ? rate_error : E_NOTIMPL;
	if (compression)
		compression->lpVtbl->Release(compression);
	if (api)
		api->lpVtbl->Release(api);
	return first;
}

HRESULT dshow_filter_load_state(void *filter, const void *data, size_t size)
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

HRESULT dshow_filter_save_state(void *filter, void **data, size_t *size)
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
