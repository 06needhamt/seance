/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Fixture modes that need a process of their own, since the fixture reads
 * them from the environment as it first sees it: the host's reaction to errors
 * a filter reports through IMediaEventSink, with the filter also supplying its
 * own allocator (whose samples it insists on getting back).
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dshow/dshow.h"
#include "loader/modules.h"
#include "runtime/runtime.h"
#include "test.h"

struct received_sample {
	int calls;
};

static HRESULT receive_sample(void *opaque, IMediaSample *sample,
			      const AM_MEDIA_TYPE *dynamic_type)
{
	((struct received_sample *)opaque)->calls++;
	return S_OK;
}

/*
 * A filter that reports a bad frame by event, and returns S_OK from Receive,
 * still fails the stream: at once for the next push, until a seek.
 */
static void fixture_notify_error(void)
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
	setenv("VFW_ENV_TESTFILTER_NOTIFY_ERROR", "1", 1);
	setenv("VFW_ENV_TESTFILTER_OWN_ALLOCATOR", "1", 1);
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
	static const BYTE good[]={'K',12,0,0,5,1,2,3,4,5,6};
	static const BYTE bad[]={'K',99,0,0,5,1,2,3,4,5,6};	/* wrong bit count */
	CHECK(host && dshow_host_push(host,good,sizeof(good),0,400000,
		AM_SAMPLE_SPLICEPOINT)==S_OK);
	CHECK(received.calls==1);
	/* Receive says S_OK; the event makes the push fail. */
	CHECK(host && dshow_host_push(host,bad,sizeof(bad),400000,800000,0)==
		VFW_E_INVALID_FILE_FORMAT);
	CHECK(received.calls==1);
	/* And it stays failed, without delivering more, until a seek. */
	CHECK(host && dshow_host_push(host,good,sizeof(good),800000,1200000,
		AM_SAMPLE_SPLICEPOINT)==VFW_E_INVALID_FILE_FORMAT);
	CHECK(received.calls==1);
	CHECK(host && dshow_host_seek(host,0,1200000)==S_OK);
	CHECK(host && dshow_host_push(host,good,sizeof(good),0,400000,
		AM_SAMPLE_SPLICEPOINT)==S_OK);
	CHECK(received.calls==2);
	CHECK(host && dshow_host_end(host,1000)==S_OK);

	/* An error that comes with the end of the stream is returned by it. */
	CHECK(host && dshow_host_seek(host,0,1200000)==S_OK);
	CHECK(host && dshow_host_push(host,bad,sizeof(bad),0,400000,
		AM_SAMPLE_SPLICEPOINT)==VFW_E_INVALID_FILE_FORMAT);
	CHECK(host && dshow_host_end(host,1000)==VFW_E_INVALID_FILE_FORMAT);
	dshow_host_destroy(host);
	unsetenv("VFW_ENV_TESTFILTER_NOTIFY_ERROR");
	unsetenv("VFW_ENV_TESTFILTER_OWN_ALLOCATOR");
	if (filter) filter->lpVtbl->Release(filter);
	dshow_free_media_type(&in_mt);
	dshow_free_media_type(&out_mt);
	typedef HRESULT (WINAPI *unload_fn)(void);
	unload_fn unload=(unload_fn)mod_get_proc(m,"DllCanUnloadNow");
	CHECK(unload && unload()==S_OK);
	mod_free(m);
}

int main(void)
{
	fixture_notify_error();
	return test_summary("test_dshow_modes");
}
