/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "dshow/dshow.h"
#include "loader/modules.h"
#include "runtime/runtime.h"
#include "test.h"

struct fake_filter {
	IBaseFilter filter;
	IAMVideoCompression compression;
	IPersistStream persist;
	ULONG refs;
	LONG key_rate, pframes;
	double quality;
	DWORDLONG window;
	BYTE state[16];
	ULONG state_size;
};

static struct fake_filter *from_filter(IBaseFilter *p)
{
	return (void *)p;
}

static struct fake_filter *from_compression(IAMVideoCompression *p)
{
	return (void *)((char *)p - offsetof(struct fake_filter, compression));
}

static struct fake_filter *from_persist(IPersistStream *p)
{
	return (void *)((char *)p - offsetof(struct fake_filter, persist));
}

static int guid_eq(REFIID a, REFIID b)
{
	return !memcmp(a, b, sizeof(*a));
}

static HRESULT WINAPI fake_qi(IBaseFilter *p, REFIID iid, void **out)
{
	struct fake_filter *f = from_filter(p);

	if (!out)
		return E_POINTER;
	if (guid_eq(iid, &IID_IUnknown) || guid_eq(iid, &IID_IBaseFilter))
		*out = &f->filter;
	else if (guid_eq(iid, &IID_IAMVideoCompression))
		*out = &f->compression;
	else if (guid_eq(iid, &IID_IPersistStream))
		*out = &f->persist;
	else {
		*out = NULL;
		return E_NOINTERFACE;
	}
	f->refs++;
	return S_OK;
}

static ULONG WINAPI fake_addref(IBaseFilter *p)
{
	return ++from_filter(p)->refs;
}

static ULONG WINAPI fake_release(IBaseFilter *p)
{
	return --from_filter(p)->refs;
}

static HRESULT WINAPI notimpl_filter(IBaseFilter *p)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI compression_qi(IAMVideoCompression *p, REFIID iid,
				     void **out)
{
	return fake_qi(&from_compression(p)->filter, iid, out);
}

static ULONG WINAPI compression_addref(IAMVideoCompression *p)
{
	return ++from_compression(p)->refs;
}

static ULONG WINAPI compression_release(IAMVideoCompression *p)
{
	return --from_compression(p)->refs;
}

static HRESULT WINAPI put_key(IAMVideoCompression *p, LONG value)
{
	from_compression(p)->key_rate = value;
	return S_OK;
}

static HRESULT WINAPI get_key(IAMVideoCompression *p, LONG *value)
{
	*value = from_compression(p)->key_rate;
	return S_OK;
}

static HRESULT WINAPI put_pframes(IAMVideoCompression *p, LONG value)
{
	from_compression(p)->pframes = value;
	return S_OK;
}

static HRESULT WINAPI get_pframes(IAMVideoCompression *p, LONG *value)
{
	*value = from_compression(p)->pframes;
	return S_OK;
}

static HRESULT WINAPI put_quality(IAMVideoCompression *p, double value)
{
	from_compression(p)->quality = value;
	return S_OK;
}

static HRESULT WINAPI get_quality(IAMVideoCompression *p, double *value)
{
	*value = from_compression(p)->quality;
	return S_OK;
}

static HRESULT WINAPI put_window(IAMVideoCompression *p, DWORDLONG value)
{
	from_compression(p)->window = value;
	return S_OK;
}

static HRESULT WINAPI get_window(IAMVideoCompression *p, DWORDLONG *value)
{
	*value = from_compression(p)->window;
	return S_OK;
}

static HRESULT WINAPI compression_info(IAMVideoCompression *p, LPWSTR version,
	int *version_size, LPWSTR description, int *description_size,
	LONG *key, LONG *pframes, double *quality, LONG *caps)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI compression_override_key(IAMVideoCompression *p,
					       LONG frame)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI compression_override_size(IAMVideoCompression *p,
						LONG frame, LONG size)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI persist_qi(IPersistStream *p, REFIID iid, void **out)
{
	return fake_qi(&from_persist(p)->filter, iid, out);
}

static ULONG WINAPI persist_addref(IPersistStream *p)
{
	return ++from_persist(p)->refs;
}

static ULONG WINAPI persist_release(IPersistStream *p)
{
	return --from_persist(p)->refs;
}

static HRESULT WINAPI persist_class(IPersistStream *p, CLSID *clsid)
{
	memset(clsid, 0, sizeof(*clsid));
	return S_OK;
}

static HRESULT WINAPI persist_dirty(IPersistStream *p)
{
	return S_FALSE;
}

static HRESULT WINAPI persist_load(IPersistStream *p, IStream *stream)
{
	struct fake_filter *f = from_persist(p);
	ULONG got = 0;

	f->state_size = 0;
	HRESULT hr = stream->lpVtbl->Read(stream, f->state, sizeof(f->state),
					  &got);
	if (SUCCEEDED(hr))
		f->state_size = got;
	return hr;
}

static HRESULT WINAPI persist_save(IPersistStream *p, IStream *stream,
				   BOOL clear_dirty)
{
	struct fake_filter *f = from_persist(p);
	ULONG written = 0;
	HRESULT hr = stream->lpVtbl->Write(stream, f->state, f->state_size,
					   &written);
	return SUCCEEDED(hr) && written != f->state_size ? E_FAIL : hr;
}

static HRESULT WINAPI persist_size(IPersistStream *p, ULARGE_INTEGER *size)
{
	size->u.LowPart = from_persist(p)->state_size;
	size->u.HighPart = 0;
	return S_OK;
}

static const IAMVideoCompressionVtbl compression_vtbl = {
	compression_qi, compression_addref, compression_release,
	put_key, get_key, put_pframes, get_pframes, put_quality, get_quality,
	put_window, get_window, compression_info, compression_override_key,
	compression_override_size,
};

static const IPersistStreamVtbl persist_vtbl = {
	persist_qi, persist_addref, persist_release, persist_class,
	persist_dirty, persist_load, persist_save, persist_size,
};

static const IBaseFilterVtbl filter_vtbl = {
	fake_qi, fake_addref, fake_release,
	(void *)notimpl_filter, (void *)notimpl_filter, (void *)notimpl_filter,
	(void *)notimpl_filter, (void *)notimpl_filter, (void *)notimpl_filter,
	(void *)notimpl_filter, (void *)notimpl_filter, (void *)notimpl_filter,
	(void *)notimpl_filter, (void *)notimpl_filter, (void *)notimpl_filter,
};

static void media_types(void)
{
	BITMAPINFOHEADER b = { .biSize = sizeof(b), .biWidth = 16,
		.biHeight = 8, .biPlanes = 1, .biBitCount = 24,
		.biCompression = BI_RGB, .biSizeImage = 16 * 8 * 3 };
	AM_MEDIA_TYPE mt, copy;
	BITMAPINFOHEADER *back = NULL;
	size_t size = 0;

	CHECK(dshow_bih_to_mt(&b, sizeof(b), 25, 1, 0, &mt) == S_OK);
	CHECK(mt.cbFormat == sizeof(VIDEOINFOHEADER));
	CHECK(mt.subtype.Data1 == MEDIASUBTYPE_RGB24.Data1);
	CHECK(((VIDEOINFOHEADER *)mt.pbFormat)->AvgTimePerFrame == 400000);
	CHECK(dshow_copy_media_type(&copy, &mt) == S_OK);
	CHECK(copy.pbFormat != mt.pbFormat);
	CHECK(dshow_mt_to_bih(&copy, &back, &size) == S_OK);
	CHECK(size == sizeof(b));
	CHECK(back->biWidth == 16 && back->biHeight == 8);
	free(back);
	dshow_free_media_type(&copy);
	dshow_free_media_type(&mt);

	CHECK(dshow_bih_to_mt(&b, sizeof(b) - 1, 25, 1, 0, &mt) ==
	      E_INVALIDARG);
}

static void malformed_media_types(void)
{
	BITMAPINFOHEADER b = { .biSize = sizeof(b), .biWidth = 16,
		.biHeight = 8, .biPlanes = 1, .biBitCount = 24,
		.biCompression = BI_RGB, .biSizeImage = 16 * 8 * 3 };
	AM_MEDIA_TYPE mt;
	BITMAPINFOHEADER *back = (void *)1;

	CHECK(dshow_bih_to_mt(&b, sizeof(b), 25, 1, 1, &mt) == S_OK);
	ULONG complete = mt.cbFormat;
	GUID not_video = { 0 };
	for (ULONG n = 0; n < complete; n++) {
		mt.cbFormat = n;
		back = (void *)1;
		CHECK(dshow_mt_to_bih(&mt, &back, NULL) ==
		      VFW_E_INVALIDMEDIATYPE);
		CHECK(back == NULL);
	}
	mt.cbFormat = complete;
	mt.majortype = not_video;
	CHECK(dshow_mt_to_bih(&mt, &back, NULL) == VFW_E_INVALIDMEDIATYPE);
	mt.majortype = MEDIATYPE_Video;
	dshow_free_media_type(&mt);

	b.biWidth = 0;
	CHECK(dshow_bih_to_mt(&b, sizeof(b), 25, 1, 0, &mt) == E_INVALIDARG);
	b.biWidth = 16;
	b.biHeight = 0;
	CHECK(dshow_bih_to_mt(&b, sizeof(b), 25, 1, 0, &mt) == E_INVALIDARG);
	b.biHeight = 8;
	b.biPlanes = 2;
	CHECK(dshow_bih_to_mt(&b, sizeof(b), 25, 1, 0, &mt) == E_INVALIDARG);
}

static void allocator(void)
{
	IMemAllocator *a;
	ALLOCATOR_PROPERTIES want = { 2, 100, 16, 4 }, got;
	IMediaSample *s[2], *none;
	BYTE *p;
	REFERENCE_TIME start = 10, stop = 20, astart, astop;

	CHECK(dshow_allocator_create(&a) == S_OK);
	CHECK(a->lpVtbl->SetProperties(a, &want, &got) == S_OK);
	CHECK(a->lpVtbl->Commit(a) == S_OK);
	CHECK(a->lpVtbl->GetBuffer(a, &s[0], &start, &stop, 0) == S_OK);
	CHECK(a->lpVtbl->GetBuffer(a, &s[1], NULL, NULL, 0) == S_OK);
	CHECK(a->lpVtbl->GetBuffer(a, &none, NULL, NULL, AM_GBF_NOWAIT) ==
	      VFW_E_TIMEOUT);
	CHECK(s[0]->lpVtbl->GetPointer(s[0], &p) == S_OK);
	CHECK(!((uintptr_t)p & 15));
	CHECK(s[0]->lpVtbl->GetTime(s[0], &astart, &astop) == S_OK);
	CHECK(astart == start && astop == stop);
	CHECK(s[0]->lpVtbl->SetActualDataLength(s[0], 101) ==
	      VFW_E_BUFFER_OVERFLOW);
	s[0]->lpVtbl->Release(s[0]);
	s[1]->lpVtbl->Release(s[1]);
	CHECK(a->lpVtbl->Decommit(a) == S_OK);
	CHECK(a->lpVtbl->Release(a) == 0);
}

static void encoder_controls(void)
{
	struct fake_filter f = {
		.filter.lpVtbl = &filter_vtbl,
		.compression.lpVtbl = &compression_vtbl,
		.persist.lpVtbl = &persist_vtbl,
		.refs = 1,
	};
	struct dshow_encoder_options options = {
		.key_interval = 12,
		.quality = 0.75,
		.bitrate = 800000,
	};
	static const BYTE state[] = { 0x54, 0x53, 0x54, 0x30, 1, 2, 3 };
	void *saved = NULL;
	size_t saved_size = 0;

	CHECK(dshow_encoder_configure(&f.filter, &options) == S_OK);
	CHECK(f.key_rate == 12);
	CHECK(f.pframes == 11);
	CHECK(f.quality == 0.75);
	CHECK(f.window == 800000);
	CHECK(f.refs == 1);

	CHECK(dshow_filter_load_state(&f.filter, state, sizeof(state)) == S_OK);
	CHECK(f.state_size == sizeof(state));
	CHECK(!memcmp(f.state, state, sizeof(state)));
	CHECK(dshow_filter_save_state(&f.filter, &saved, &saved_size) == S_OK);
	CHECK(saved_size == sizeof(state));
	CHECK(!memcmp(saved, state, sizeof(state)));
	CHECK(f.refs == 1);
	free(saved);
}

static void call_wrapper_arguments(void)
{
	FILTER_STATE state;
	ALLOCATOR_PROPERTIES props;
	IMemAllocator *allocator = (void *)1;
	LONG processed;

	CHECK(dshow_call_filter_get_state(NULL, 0, &state) == E_POINTER);
	CHECK(dshow_call_filter_set_sync_source(NULL, NULL) == E_POINTER);
	CHECK(dshow_call_filter_join_graph(NULL, NULL, NULL) == E_POINTER);
	CHECK(dshow_call_pin_begin_flush(NULL) == E_POINTER);
	CHECK(dshow_call_pin_end_flush(NULL) == E_POINTER);
	CHECK(dshow_call_pin_new_segment(NULL, 0, 1, 1.0) == E_POINTER);
	CHECK(dshow_call_pin_query_accept(NULL, NULL) == E_POINTER);
	CHECK(dshow_call_mem_get_allocator(NULL, &allocator) == E_POINTER);
	CHECK(allocator == (void *)1); /* A NULL pin cannot touch the result. */
	CHECK(dshow_call_mem_notify_allocator(NULL, NULL, FALSE) == E_POINTER);
	CHECK(dshow_call_mem_allocator_requirements(NULL, &props) == E_POINTER);
	CHECK(dshow_call_mem_receive_multiple(NULL, NULL, 0, &processed) ==
	      E_POINTER);
	CHECK(dshow_call_mem_receive_can_block(NULL) == E_POINTER);
}

static void host_arguments(void)
{
	struct dshow_host *host = (void *)1;
	LONG code;
	LONG_PTR param1, param2;

	CHECK(dshow_host_create(NULL, NULL, NULL, NULL, NULL, &host) == E_POINTER);
	CHECK(dshow_host_push(NULL, NULL, 0, 0, 0, 0) == E_POINTER);
	CHECK(dshow_host_seek(NULL, 0, 0) == E_POINTER);
	CHECK(dshow_host_end(NULL, 1) == E_POINTER);
	CHECK(dshow_host_event(NULL, &code, &param1, &param2) == E_POINTER);
	dshow_host_destroy(NULL);
}

struct received_sample {
	BYTE data[32];
	size_t size;
	int calls;
};

static HRESULT receive_sample(void *opaque, IMediaSample *sample,
			      const AM_MEDIA_TYPE *dynamic_type)
{
	struct received_sample *r = opaque;
	BYTE *data = NULL;
	LONG size = sample->lpVtbl->GetActualDataLength(sample);
	if (size < 0 || size > (LONG)sizeof(r->data) ||
	    FAILED(sample->lpVtbl->GetPointer(sample, &data)))
		return E_FAIL;
	memcpy(r->data, data, size);
	r->size = size;
	r->calls++;
	return S_OK;
}

static void fixture_host(void)
{
	const char *dir = getenv("VFW_FIXTURES");
	char path[4096];
	if (!dir) return;
	snprintf(path, sizeof(path), "%s/testfilter.ax", dir);
	if (access(path, R_OK)) return;
	CHECK(runtime_enter() == 0);
	struct module *m = mod_load(path, NULL);
	CHECK(m != NULL);
	if (!m) return;
	LPFNGETCLASSOBJECT get = (LPFNGETCLASSOBJECT)mod_get_proc(m,
		"DllGetClassObject");
	static const CLSID decoder = { 0x5e6c9b31, 0x3a7d, 0x4f0e,
		{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };
	IClassFactory *factory = NULL;
	IBaseFilter *filter = NULL;
	CHECK(get != NULL);
	CHECK(get && get(&decoder, &IID_IClassFactory, (void **)&factory) == S_OK);
	CHECK(factory && factory->lpVtbl->CreateInstance(factory, NULL,
		&IID_IBaseFilter, (void **)&filter) == S_OK);
	if (factory) factory->lpVtbl->Release(factory);

	BITMAPINFOHEADER input = { .biSize=sizeof(input), .biWidth=2,
		.biHeight=2, .biPlanes=1, .biBitCount=12,
		.biCompression=mmioFOURCC('T','S','T','0'), .biSizeImage=16 };
	BITMAPINFOHEADER output = input;
	output.biCompression = mmioFOURCC('I','4','2','0');
	output.biSizeImage = 6;
	AM_MEDIA_TYPE in_mt, out_mt;
	CHECK(dshow_bih_to_mt(&input,sizeof(input),25,1,0,&in_mt)==S_OK);
	CHECK(dshow_bih_to_mt(&output,sizeof(output),25,1,0,&out_mt)==S_OK);
	struct received_sample received = {0};
	struct dshow_host *host = NULL;
	CHECK(filter && dshow_host_create(filter,&in_mt,&out_mt,receive_sample,
		&received,&host)==S_OK);
	static const BYTE compressed[]={'K',12,0,0,5,1,2,3,4,5,6};
	CHECK(host && dshow_host_push(host,compressed,sizeof(compressed),0,400000,
		AM_SAMPLE_SPLICEPOINT)==S_OK);
	CHECK(received.calls==1 && received.size==6);
	CHECK(!memcmp(received.data,compressed+5,6));
	CHECK(host && dshow_host_end(host,1000)==S_OK);
	dshow_host_destroy(host);
	dshow_free_media_type(&in_mt); dshow_free_media_type(&out_mt);
	if(filter)filter->lpVtbl->Release(filter);
	typedef HRESULT (WINAPI *unload_fn)(void);
	unload_fn unload=(unload_fn)mod_get_proc(m,"DllCanUnloadNow");
	CHECK(unload && unload()==S_OK);
	mod_free(m);
}

int main(void)
{
	media_types();
	malformed_media_types();
	allocator();
	encoder_controls();
	call_wrapper_arguments();
	host_arguments();
	fixture_host();
	return test_summary("test_dshow");
}
