/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A DirectShow filter server, the way a codec's .ax is one: four classes, a
 * TST0 decoder registered through IFilterMapper2 and a TST0 encoder
 * registered through the legacy IFilterMapper, and a TSTA audio decoder and
 * encoder (the encoder in the audio compressor category), each with its
 * HKCR\CLSID\{...}\InprocServer32 key, as DllRegisterServer writes them
 * for real filters; DllUnregisterServer takes it all away again.
 *
 * The filter objects and their transforms live in testfilter_filter.c.
 */
#include "fixture.h"
#include <strmif.h>

/* {5E6C9B31-3A7D-4F0E-9C11-0D2B7A6E4F80}: the decoder */
static const GUID clsid_decoder = { 0x5e6c9b31, 0x3a7d, 0x4f0e,
	{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };
/* {5E6C9B32-3A7D-4F0E-9C11-0D2B7A6E4F80}: the encoder */
static const GUID clsid_encoder = { 0x5e6c9b32, 0x3a7d, 0x4f0e,
	{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };
/* {5E6C9B33-3A7D-4F0E-9C11-0D2B7A6E4F80}: the audio decoder */
static const GUID clsid_audio_decoder = { 0x5e6c9b33, 0x3a7d, 0x4f0e,
	{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };
/* {5E6C9B34-3A7D-4F0E-9C11-0D2B7A6E4F80}: the audio encoder */
static const GUID clsid_audio_encoder = { 0x5e6c9b34, 0x3a7d, 0x4f0e,
	{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };
/* CLSID_AudioCompressorCategory */
static const GUID audio_compressors = { 0x33d9a761, 0x90c8, 0x11d0,
	{ 0xbd, 0x43, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86 } };

/* Windows' GUIDs, defined here: the fixture links no uuid library. */
static const GUID iid_unknown = { 0x00000000, 0x0000, 0x0000,
	{ 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID iid_factory = { 0x00000001, 0x0000, 0x0000,
	{ 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID clsid_mapper = { 0xe436ebb2, 0x524f, 0x11ce,
	{ 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
static const GUID iid_mapper = { 0x56a868a3, 0x0ad4, 0x11ce,
	{ 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
static const GUID clsid_mapper2 = { 0xcda42200, 0xbd88, 0x11d0,
	{ 0xbd, 0x4e, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86 } };
static const GUID iid_mapper2 = { 0xb79bb0b0, 0x33c1, 0x11d1,
	{ 0xab, 0xe1, 0x00, 0xa0, 0xc9, 0x05, 0xf3, 0x75 } };
static const GUID guid_null;

/* Media types: 'vids', TST0, YUY2 (FOURCC-shaped) and RGB24. */
#define FOURCC_GUID(a, b, c, d) { \
	(DWORD)(a) | (DWORD)(b) << 8 | (DWORD)(c) << 16 | (DWORD)(d) << 24, \
	0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } }
static const GUID mt_video = FOURCC_GUID('v', 'i', 'd', 's');
static const GUID st_tst0 = FOURCC_GUID('T', 'S', 'T', '0');
static const GUID st_yuy2 = FOURCC_GUID('Y', 'U', 'Y', '2');
static const GUID st_rgb24 = { 0xe436eb7d, 0x524f, 0x11ce,
	{ 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
/* 'auds', PCM and TSTA (format tag subtypes). */
static const GUID mt_audio = FOURCC_GUID('a', 'u', 'd', 's');
static const GUID st_pcm = { 0x00000001, 0x0000, 0x0010,
	{ 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID st_tsta = { 0x00007354, 0x0000, 0x0010,
	{ 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

static const WCHAR decoder_name[] = L"seance test decoder";
static const WCHAR encoder_name[] = L"seance test encoder";
static const WCHAR audio_decoder_name[] = L"seance test audio decoder";
static const WCHAR audio_encoder_name[] = L"seance test audio encoder";

static HINSTANCE self;
HRESULT testfilter_create(BOOL encoder, BOOL audio, REFIID riid, void **out);
LONG testfilter_objects(void);

static int guid_eq(const GUID *a, const GUID *b)
{
	const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;
	for (int i = 0; i < 16; i++)
		if (x[i] != y[i])
			return 0;
	return 1;
}

static int wlen(const WCHAR *s)
{
	int n = 0;
	while (s[n])
		n++;
	return n;
}

static void wcat(WCHAR *d, const WCHAR *s)
{
	d += wlen(d);
	while ((*d++ = *s++))
		;
}

/* "CLSID\{clsid}" plus suffix, in out (at least 80 WCHARs). */
static void class_key(WCHAR *out, const GUID *clsid, const WCHAR *suffix)
{
	WCHAR g[40];

	StringFromGUID2(clsid, g, 40);
	out[0] = 0;
	wcat(out, L"CLSID\\");
	wcat(out, g);
	wcat(out, suffix);
}

static LONG set_string(HKEY k, const WCHAR *name, const WCHAR *s)
{
	return RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)s,
			      (DWORD)(wlen(s) + 1) * sizeof(WCHAR));
}

/* HKCR\CLSID\{clsid}, named, with this DLL as its InprocServer32. */
static HRESULT register_class(const GUID *clsid, const WCHAR *name)
{
	WCHAR key[100], path[MAX_PATH];
	HKEY k;

	if (!GetModuleFileNameW(self, path, MAX_PATH))
		return E_FAIL;
	class_key(key, clsid, L"");
	if (RegCreateKeyExW(HKEY_CLASSES_ROOT, key, 0, NULL, 0, KEY_ALL_ACCESS,
			    NULL, &k, NULL))
		return E_FAIL;
	LONG err = set_string(k, NULL, name);
	RegCloseKey(k);
	class_key(key, clsid, L"\\InprocServer32");
	if (err || RegCreateKeyExW(HKEY_CLASSES_ROOT, key, 0, NULL, 0,
				   KEY_ALL_ACCESS, NULL, &k, NULL))
		return E_FAIL;
	err = set_string(k, NULL, path);
	if (!err)
		err = set_string(k, L"ThreadingModel", L"Both");
	RegCloseKey(k);
	return err ? E_FAIL : S_OK;
}

static void unregister_class(const GUID *clsid)
{
	WCHAR key[100];

	class_key(key, clsid, L"\\InprocServer32");
	RegDeleteKeyW(HKEY_CLASSES_ROOT, key);
	class_key(key, clsid, L"");
	RegDeleteKeyW(HKEY_CLASSES_ROOT, key);
}

/* The decoder: TST0 in, YUY2 or RGB24 out. */
static const REGPINTYPES decoder_in[] = { { &mt_video, &st_tst0 } };
static const REGPINTYPES decoder_out[] = {
	{ &mt_video, &st_yuy2 }, { &mt_video, &st_rgb24 },
};
static const REGFILTERPINS2 decoder_pins[] = {
	{ 0, 1, 1, decoder_in, 0, NULL, NULL },
	{ REG_PINFLAG_B_OUTPUT, 1, 2, decoder_out, 0, NULL, NULL },
};

static HRESULT register_decoder(void)
{
	IFilterMapper2 *fm;
	REGFILTER2 rf2;

	HRESULT hr = CoCreateInstance(&clsid_mapper2, NULL, CLSCTX_INPROC_SERVER,
				      &iid_mapper2, (void **)&fm);
	if (FAILED(hr))
		return hr;
	rf2.dwVersion = 2;
	rf2.dwMerit = MERIT_NORMAL;
	rf2.cPins2 = 2;
	rf2.rgPins2 = decoder_pins;
	hr = fm->lpVtbl->RegisterFilter(fm, &clsid_decoder, decoder_name, NULL,
					NULL, NULL, &rf2);
	fm->lpVtbl->Release(fm);
	return hr;
}

/* The encoder, the old way: YUY2 or RGB24 in, TST0 out. */
static HRESULT register_encoder(void)
{
	IFilterMapper *fm;

	HRESULT hr = CoCreateInstance(&clsid_mapper, NULL, CLSCTX_INPROC_SERVER,
				      &iid_mapper, (void **)&fm);
	if (FAILED(hr))
		return hr;
	if (SUCCEEDED(hr = fm->lpVtbl->RegisterFilter(fm, clsid_encoder,
						      encoder_name,
						      MERIT_DO_NOT_USE)) &&
	    SUCCEEDED(hr = fm->lpVtbl->RegisterPin(fm, clsid_encoder, L"Input",
						   FALSE, FALSE, FALSE, FALSE,
						   guid_null, NULL)) &&
	    SUCCEEDED(hr = fm->lpVtbl->RegisterPinType(fm, clsid_encoder,
						       L"Input", mt_video,
						       st_yuy2)) &&
	    SUCCEEDED(hr = fm->lpVtbl->RegisterPinType(fm, clsid_encoder,
						       L"Input", mt_video,
						       st_rgb24)) &&
	    SUCCEEDED(hr = fm->lpVtbl->RegisterPin(fm, clsid_encoder,
						   L"Output", FALSE, TRUE,
						   FALSE, FALSE, guid_null,
						   NULL)))
		hr = fm->lpVtbl->RegisterPinType(fm, clsid_encoder, L"Output",
						 mt_video, st_tst0);
	fm->lpVtbl->Release(fm);
	return hr;
}

/* The audio filters, through IFilterMapper2: TSTA <-> PCM. */
static const REGPINTYPES tsta_types[] = { { &mt_audio, &st_tsta } };
static const REGPINTYPES pcm_types[] = { { &mt_audio, &st_pcm } };
static const REGFILTERPINS2 audio_decoder_pins[] = {
	{ 0, 1, 1, tsta_types, 0, NULL, NULL },
	{ REG_PINFLAG_B_OUTPUT, 1, 1, pcm_types, 0, NULL, NULL },
};
static const REGFILTERPINS2 audio_encoder_pins[] = {
	{ 0, 1, 1, pcm_types, 0, NULL, NULL },
	{ REG_PINFLAG_B_OUTPUT, 1, 1, tsta_types, 0, NULL, NULL },
};

static HRESULT register_audio(void)
{
	IFilterMapper2 *fm;
	REGFILTER2 rf2;

	HRESULT hr = CoCreateInstance(&clsid_mapper2, NULL, CLSCTX_INPROC_SERVER,
				      &iid_mapper2, (void **)&fm);
	if (FAILED(hr))
		return hr;
	rf2.dwVersion = 2;
	rf2.dwMerit = MERIT_NORMAL;
	rf2.cPins2 = 2;
	rf2.rgPins2 = audio_decoder_pins;
	hr = fm->lpVtbl->RegisterFilter(fm, &clsid_audio_decoder,
					audio_decoder_name, NULL, NULL, NULL,
					&rf2);
	if (SUCCEEDED(hr)) {
		rf2.dwMerit = MERIT_DO_NOT_USE;
		rf2.rgPins2 = audio_encoder_pins;
		hr = fm->lpVtbl->RegisterFilter(fm, &clsid_audio_encoder,
						audio_encoder_name, NULL,
						&audio_compressors, NULL, &rf2);
	}
	fm->lpVtbl->Release(fm);
	return hr;
}

HRESULT WINAPI DllRegisterServer(void)
{
	HRESULT hr = CoInitialize(NULL);
	if (FAILED(hr))
		return hr;
	if (SUCCEEDED(hr = register_class(&clsid_decoder, decoder_name)) &&
	    SUCCEEDED(hr = register_class(&clsid_encoder, encoder_name)) &&
	    SUCCEEDED(hr = register_class(&clsid_audio_decoder,
					  audio_decoder_name)) &&
	    SUCCEEDED(hr = register_class(&clsid_audio_encoder,
					  audio_encoder_name)) &&
	    SUCCEEDED(hr = register_decoder()) &&
	    SUCCEEDED(hr = register_encoder()))
		hr = register_audio();
	CoUninitialize();
	return hr;
}

HRESULT WINAPI DllUnregisterServer(void)
{
	IFilterMapper2 *fm2;
	IFilterMapper *fm;

	HRESULT hr = CoInitialize(NULL);
	if (FAILED(hr))
		return hr;
	if (SUCCEEDED(CoCreateInstance(&clsid_mapper2, NULL,
				       CLSCTX_INPROC_SERVER, &iid_mapper2,
				       (void **)&fm2))) {
		fm2->lpVtbl->UnregisterFilter(fm2, NULL, NULL, &clsid_decoder);
		fm2->lpVtbl->UnregisterFilter(fm2, NULL, NULL,
					      &clsid_audio_decoder);
		fm2->lpVtbl->UnregisterFilter(fm2, &audio_compressors, NULL,
					      &clsid_audio_encoder);
		fm2->lpVtbl->Release(fm2);
	}
	if (SUCCEEDED(CoCreateInstance(&clsid_mapper, NULL,
				       CLSCTX_INPROC_SERVER, &iid_mapper,
				       (void **)&fm))) {
		fm->lpVtbl->UnregisterFilter(fm, clsid_encoder);
		fm->lpVtbl->Release(fm);
	}
	unregister_class(&clsid_decoder);
	unregister_class(&clsid_encoder);
	unregister_class(&clsid_audio_decoder);
	unregister_class(&clsid_audio_encoder);
	CoUninitialize();
	return S_OK;
}

struct factory { IClassFactory iface; BOOL encoder, audio; };
static struct factory *factory_from(IClassFactory *cf) { return (void *)cf; }

/* Class factories: one per transform filter. */
static HRESULT WINAPI cf_qi(IClassFactory *cf, REFIID riid, void **ppv)
{
	if (guid_eq(riid, &iid_unknown) || guid_eq(riid, &iid_factory)) {
		*ppv = cf;
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI cf_addref(IClassFactory *cf) { return 2; }
static ULONG WINAPI cf_release(IClassFactory *cf) { return 1; }

static HRESULT WINAPI cf_create(IClassFactory *cf, IUnknown *outer,
				REFIID riid, void **ppv)
{
	*ppv = NULL;
	return outer ? CLASS_E_NOAGGREGATION :
		testfilter_create(factory_from(cf)->encoder,
				  factory_from(cf)->audio, riid, ppv);
}

static HRESULT WINAPI cf_lock(IClassFactory *cf, BOOL lock) { return S_OK; }

static const IClassFactoryVtbl cf_vtbl = {
	cf_qi, cf_addref, cf_release, cf_create, cf_lock
};
static struct factory decoder_factory = {
	{ (IClassFactoryVtbl *)&cf_vtbl }, FALSE
};
static struct factory encoder_factory = {
	{ (IClassFactoryVtbl *)&cf_vtbl }, TRUE
};
static struct factory audio_decoder_factory = {
	{ (IClassFactoryVtbl *)&cf_vtbl }, FALSE, TRUE
};
static struct factory audio_encoder_factory = {
	{ (IClassFactoryVtbl *)&cf_vtbl }, TRUE, TRUE
};

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
	if (guid_eq(clsid, &clsid_decoder))
		return cf_qi(&decoder_factory.iface, riid, ppv);
	if (guid_eq(clsid, &clsid_encoder))
		return cf_qi(&encoder_factory.iface, riid, ppv);
	if (guid_eq(clsid, &clsid_audio_decoder))
		return cf_qi(&audio_decoder_factory.iface, riid, ppv);
	if (guid_eq(clsid, &clsid_audio_encoder))
		return cf_qi(&audio_encoder_factory.iface, riid, ppv);
	*ppv = NULL;
	return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
	return testfilter_objects() ? S_FALSE : S_OK;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		self = inst;
	return TRUE;
}
